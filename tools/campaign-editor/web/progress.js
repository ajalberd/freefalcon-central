'use strict';

// Progress tab: how the war is going, from each team's own statistics (the
// .tea member) and a roll-up of its units and objectives. One file is a
// snapshot; every save of the same scenario, ordered by campaign time, makes
// the lines.

const SVGNS = 'http://www.w3.org/2000/svg';
const svg = (tag, attrs, kids) => {
  const n = document.createElementNS(SVGNS, tag);
  for (const k in (attrs || {})) n.setAttribute(k, attrs[k]);
  for (const kid of [].concat(kids || [])) if (kid) n.appendChild(kid);
  return n;
};

// Colours by team index, matching the map's swatches where possible.
const progressColour = team => (typeof teamColour === 'function'
  ? teamColour(team) : ['#888', '#4da3ff', '#3fb950', '#f2a33c', '#e5484d',
                        '#c77dff', '#ff6b5e', '#aaa'][team] || '#888');

function lineChart(title, series, teams, key, opts) {
  opts = opts || {};
  const W = 460, H = 200, L = 44, R = 12, T = 12, B = 30;
  const pts = series.filter(p => teams.some(t => p.teams[t.team]));
  const box = el('div', {class: 'pg-chart'}, el('h4', {text: title}));
  if (pts.length < 2) {
    box.appendChild(el('p', {class: 'note', text:
      'Needs two or more saves of this campaign to draw a line.'}));
    return box;
  }
  const t0 = pts[0].time, t1 = pts[pts.length - 1].time || t0 + 1;
  let lo = Infinity, hi = -Infinity;
  for (const p of pts) for (const t of teams) {
    const v = p.teams[t.team] && p.teams[t.team][key];
    if (v === undefined) continue;
    lo = Math.min(lo, v); hi = Math.max(hi, v);
  }
  if (opts.zero) lo = Math.min(0, lo);
  if (hi === lo) { hi += 1; lo = Math.max(0, lo - 1); }
  const pad = (hi - lo) * 0.08;
  hi += pad; if (!opts.zero) lo -= pad;
  const X = t => L + (W - L - R) * ((t - t0) / Math.max(1, t1 - t0));
  const Y = v => T + (H - T - B) * (1 - (v - lo) / (hi - lo));

  const g = svg('svg', {viewBox: '0 0 ' + W + ' ' + H, class: 'pg-svg'});
  for (let i = 0; i <= 4; i++) {
    const v = lo + (hi - lo) * i / 4, y = Y(v);
    g.appendChild(svg('line', {x1: L, x2: W - R, y1: y, y2: y,
                               class: 'pg-grid'}));
    const lab = svg('text', {x: L - 6, y: y + 4, class: 'pg-axis',
                             'text-anchor': 'end'});
    lab.textContent = Math.round(v).toLocaleString();
    g.appendChild(lab);
  }
  const first = svg('text', {x: L, y: H - 10, class: 'pg-axis'});
  first.textContent = pts[0].clock;
  const last = svg('text', {x: W - R, y: H - 10, class: 'pg-axis',
                            'text-anchor': 'end'});
  last.textContent = pts[pts.length - 1].clock;
  g.appendChild(first);
  g.appendChild(last);

  for (const t of teams) {
    const line = pts.filter(p => p.teams[t.team] &&
                                 p.teams[t.team][key] !== undefined);
    if (!line.length) continue;
    const d = line.map((p, i) => (i ? 'L' : 'M') + X(p.time).toFixed(1) +
                                 ' ' + Y(p.teams[t.team][key]).toFixed(1))
      .join(' ');
    g.appendChild(svg('path', {d: d, fill: 'none', stroke: progressColour(t.team),
                               'stroke-width': 2}));
    for (const p of line) {
      const c = svg('circle', {cx: X(p.time), cy: Y(p.teams[t.team][key]),
                               r: 3, fill: progressColour(t.team)});
      const tip = svg('title');
      tip.textContent = t.name + ' · ' + p.clock + ' · ' +
        p.teams[t.team][key].toLocaleString() + ' · ' + p.file;
      c.appendChild(tip);
      g.appendChild(c);
    }
  }
  box.appendChild(g);
  return box;
}

function statBar(label, cur, start, suffix) {
  const pct = start ? Math.round(100 * cur / start) : null;
  const hue = pct === null ? 0 : Math.round(Math.max(0, Math.min(100, pct)) * 1.2);
  return el('div', {class: 'pg-stat'}, [
    el('div', {class: 'pg-stat-lab', text: label}),
    el('div', {class: 'pg-stat-track'}, el('div', {class: 'pg-stat-fill',
      style: 'width:' + Math.max(0, Math.min(100, pct === null ? 0 : pct)) +
             '%;background:hsl(' + hue + ',55%,45%)'})),
    el('div', {class: 'pg-stat-val', text: cur.toLocaleString() +
      (start ? ' / ' + start.toLocaleString() : '') + (suffix || '') +
      (pct !== null ? '  (' + pct + '%)' : '')}),
  ]);
}

function teamCard(t, labels) {
  const u = t.units || {};
  const cur = t.current, st = t.start;
  const losses = ['aircraft', 'groundVehs', 'airDefenseVehs', 'ships']
    .map(k => st[k] ? cur[k] / st[k] : null).filter(v => v !== null);
  const overall = losses.length
    ? Math.round(100 * losses.reduce((a, b) => a + b, 0) / losses.length) : null;

  const held = Object.entries(t.held || {}).sort((a, b) => b[1] - a[1]);
  return el('div', {class: 'panel pg-team'}, [
    el('header', {}, [
      el('h3', {}, [el('span', {class: 'swatch',
        style: 'background:' + progressColour(t.team)}), ' ' + t.name]),
      el('span', {class: 'hint', text: overall === null ? ''
        : 'forces at ' + overall + '% of their start'}),
    ]),
    el('div', {class: 'panel-body'}, [
      ...['aircraft', 'groundVehs', 'airDefenseVehs', 'ships', 'airbases']
        .filter(k => st[k] || cur[k])
        .map(k => statBar(labels[k], cur[k], st[k])),
      el('div', {class: 'pg-kv'}, [
        el('span', {text: 'Initiative'}), el('b', {text: String(t.initiative)}),
        el('span', {text: 'Supply / fuel level'}),
        el('b', {text: cur.supplyLevel + '% / ' + cur.fuelLevel + '%'}),
        el('span', {text: 'Pools: supply · fuel · replacements'}),
        el('b', {text: t.supplyAvail + ' · ' + t.fuelAvail + ' · ' +
                       t.replacementsAvail}),
        u.battalions !== undefined ? el('span', {text: 'Battalions: health · supply · morale · fatigue'}) : null,
        u.battalions !== undefined ? el('b', {text: u.battalions + ' units: ' +
          [u.health, u.supply, u.morale, u.fatigue].map(v =>
            v === null || v === undefined ? '–' : v + '%').join(' · ')}) : null,
        el('span', {text: 'Squadrons · task forces'}),
        el('b', {text: (u.squadrons || 0) + ' · ' + (u.taskforces || 0)}),
      ]),
      held.length ? el('div', {class: 'pg-held'}, [
        el('div', {class: 'pg-stat-lab', text: 'Objectives held'}),
        el('div', {class: 'pg-chips'}, held.map(([k, v]) =>
          el('span', {class: 'pill', text: k + ' ' + v}))),
      ]) : null,
    ]),
  ]);
}

async function progressPanel(file) {
  const d = await api('/api/progress?' + qs({file: file}));
  const wrap = el('div', {class: 'pad'});

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: 'Campaign progress — ' + file}),
      el('span', {class: 'hint', text: d.clock + '   ·   scenario ' +
        (d.scenario || '?') + '   ·   ' + d.series.length + ' save' +
        (d.series.length === 1 ? '' : 's') + ' of it in this folder'}),
    ]),
    el('div', {class: 'panel-body'}, [
      el('p', {class: 'note', text:
        'Each team’s own count of what it has left against what it ' +
        'started with, as the game keeps it. The pools are the supply, fuel ' +
        'and replacements its production makes to hand out; initiative is ' +
        'who is setting the pace of the ground war (0–100). The lines ' +
        'join every save of this scenario by campaign time, so saves from ' +
        'two different runs of it will be drawn as one.'}),
      d.skipped && d.skipped.length ? el('p', {class: 'note warn', text:
        'Left out of the lines: ' + d.skipped.join(', ') + ' — its ' +
        'forces add up to under a tenth of the start, which is a save of a ' +
        'broken or half-loaded campaign rather than a real result.'}) : null,
      d.unitsError ? el('p', {class: 'note warn', text:
        'The unit list of this file did not read (' + d.unitsError +
        '), so the unit averages are missing.'}) : null,
    ]),
  ]));

  if (!d.teams.length) {
    wrap.appendChild(el('p', {class: 'note', text:
      'No team statistics in this file yet — a scenario that has not ' +
      'been played has none until the game first runs it.'}));
    return wrap;
  }

  // Enemies side by side: each warring team, then whoever it is at war with,
  // so Korea reads ROK | DPRK. Teams at war with nobody are hidden unless
  // asked for -- China and the CIS are in the file, neutral, untouched.
  const byTeam = new Map(d.teams.map(t => [t.team, t]));
  const ordered = [];
  for (const t of d.teams) {
    if (!t.atWarWith.length || ordered.includes(t)) continue;
    ordered.push(t);
    for (const e of t.atWarWith) {
      const o = byTeam.get(e);
      if (o && !ordered.includes(o)) ordered.push(o);
    }
  }
  const idle = d.teams.filter(t => !t.atWarWith.length);
  let showIdle = false;
  try { showIdle = localStorage.getItem('ffcamp.progress.idle') === '1'; }
  catch (e) { /* no storage: default off */ }
  const visible = showIdle ? ordered.concat(idle) : ordered;

  if (idle.length) {
    wrap.appendChild(el('label', {class: 'chk pg-idle'}, [
      el('input', {type: 'checkbox', checked: showIdle, onchange: e => {
        try { localStorage.setItem('ffcamp.progress.idle',
                                   e.target.checked ? '1' : '0'); }
        catch (err) { /* ignore */ }
        drawView();
      }}),
      el('span', {text: 'Show teams not at war (' +
                        idle.map(t => t.name).join(', ') + ')'}),
    ]));
  }

  wrap.appendChild(el('div', {class: 'pg-teams'},
                      visible.map(t => teamCard(t, d.labels))));

  const teams = visible.map(t => ({team: t.team, name: t.name}));
  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [el('h3', {text: 'Over the campaign'}),
      el('span', {class: 'legend-inline'}, teams.map(t =>
        el('span', {}, [el('span', {class: 'swatch',
          style: 'background:' + progressColour(t.team)}), ' ' + t.name + '  '])))]),
    el('div', {class: 'panel-body pg-charts'}, [
      lineChart('Aircraft', d.series, teams, 'aircraft'),
      lineChart('Ground vehicles', d.series, teams, 'groundVehs'),
      lineChart('Air defence vehicles', d.series, teams, 'airDefenseVehs'),
      lineChart('Initiative', d.series, teams, 'initiative', {zero: true}),
      lineChart('Supply pool', d.series, teams, 'supplyAvail', {zero: true}),
      lineChart('Fuel pool', d.series, teams, 'fuelAvail', {zero: true}),
      lineChart('Replacements pool', d.series, teams, 'replacementsAvail',
                {zero: true}),
      lineChart('Ships', d.series, teams, 'ships'),
    ]),
  ]));
  return wrap;
}
