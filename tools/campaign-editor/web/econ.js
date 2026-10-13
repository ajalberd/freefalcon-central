'use strict';
// Economy layers for the campaign map: Power, Production, Supply.
//
// They show what ProduceSupplies / IsSupplySource do with the objectives (see CAMPAIGN-SUPPLY-ENGINE.md):
//   output = DataRate x damage% (objective status) x status% of the nearest friendly power plant
// Army bases, depots, factories and ports feed supply and replacements, refineries feed fuel, power plants scale all of
// them; cities, ports, depots and army bases are where a delivery starts (unless they are on the front line, which a
// static file cannot say). The rate and status come from the server with every objective (api_map "rate" / "status").
//
// Approximation: the engine's "friendly" plant is the nearest one of a team that is neutral or friendly to the producer;
// the file does not carry relations, so this uses the nearest plant with the same owner.

const ECON_KIND = {
  supply: new Set([3, 10, 11, 19]),     // army base, depot, factory, port
  fuel: new Set([25]),                  // refinery
  power: new Set([17, 20]),             // nuclear plant, power plant
  source: new Set([3, 8, 10, 19]),      // city, port, depot, army base
};
const ECON_COL = {supply: '#ffb347', fuel: '#5ee6a8', power: '#ffe14d', source: '#6ad1ff'};

function econStatusColour(pct, alpha) {
  const a = alpha === undefined ? 1 : alpha;
  if (pct >= 90) return 'rgba(120,230,150,' + a + ')';
  if (pct >= 60) return 'rgba(255,214,90,' + a + ')';
  if (pct >= 30) return 'rgba(255,150,60,' + a + ')';
  return 'rgba(255,70,70,' + a + ')';
}

function econBuild(data) {
  if (data._econ) return data._econ;
  const plants = data.objectives.filter(o => ECON_KIND.power.has(o.t));
  const byOwner = new Map();
  for (const p of plants) {
    if (!byOwner.has(p.owner)) byOwner.set(p.owner, []);
    byOwner.get(p.owner).push(p);
  }
  const producers = [], fed = new Map();
  for (const o of data.objectives) {
    const supply = ECON_KIND.supply.has(o.t), fuel = ECON_KIND.fuel.has(o.t);
    if ((!supply && !fuel) || !o.rate) continue;
    let plant = null, bd = Infinity;
    for (const p of (byOwner.get(o.owner) || [])) {
      const d = (p.x - o.x) ** 2 + (p.y - o.y) ** 2;
      if (d < bd) { bd = d; plant = p; }
    }
    // A team with no plant of its own (PRC, CIS in Korea) would use a neutral or allied team's in the engine, which the file
    // cannot name: assume full power and say so.
    const power = plant ? plant.status : 100;
    const rec = {o, plant, noPlant: !plant, kind: fuel ? 'fuel' : 'supply', km: plant ? Math.sqrt(bd) : 0,
                 full: o.rate, damaged: o.rate * o.status / 100, eff: o.rate * o.status / 100 * power / 100};
    producers.push(rec);
    o._econ = rec;
    if (plant) fed.set(plant, (fed.get(plant) || 0) + 1);
  }
  const totals = new Map();
  const row = owner => {
    if (!totals.has(owner)) totals.set(owner, {
      supply: {n: 0, full: 0, damaged: 0, eff: 0}, fuel: {n: 0, full: 0, damaged: 0, eff: 0},
      plants: 0, plantSum: 0, sources: 0, bridges: 0, bridgesHit: 0});
    return totals.get(owner);
  };
  for (const r of producers) {
    const t = row(r.o.owner)[r.kind];
    t.n++; t.full += r.full; t.damaged += r.damaged; t.eff += r.eff;
    if (r.noPlant) t.assumed = true;
  }
  for (const p of plants) { const t = row(p.owner); t.plants++; t.plantSum += p.status; }
  const sources = data.objectives.filter(o => ECON_KIND.source.has(o.t));
  for (const o of sources) row(o.owner).sources++;
  const bridges = data.objectives.filter(o => o.t === 6);
  for (const b of bridges) { const t = row(b.owner); t.bridges++; if (b.status < 100) t.bridgesHit++; }
  return (data._econ = {plants, producers, fed, sources, bridges, totals});
}

function drawEcon(g, r, toScreen, scale, M, data) {
  const E = econBuild(data);
  const on = (sx, sy, m) => sx > -m && sy > -m && sx < r.width + m && sy < r.height + m;
  const zoom = Math.min(2.2, Math.max(0.7, Math.sqrt(scale)));
  g.save();

  if (M.showPower) {                                   // each producer to the plant that feeds it
    g.lineWidth = 1;
    for (const rec of E.producers) {
      if (!rec.plant) continue;
      const [ax, ay] = toScreen(rec.o.x, rec.o.y), [bx, by] = toScreen(rec.plant.x, rec.plant.y);
      if (!on(ax, ay, 60) && !on(bx, by, 60)) continue;
      g.strokeStyle = econStatusColour(rec.plant.status, rec.plant.status >= 90 ? 0.28 : 0.7);
      g.beginPath(); g.moveTo(ax, ay); g.lineTo(bx, by); g.stroke();
    }
    for (const p of E.plants) {
      const [sx, sy] = toScreen(p.x, p.y);
      if (!on(sx, sy, 30)) continue;
      const n = E.fed.get(p) || 0, rad = (6 + Math.sqrt(n) * 1.6) * zoom;
      g.beginPath(); g.arc(sx, sy, rad, 0, Math.PI * 2);
      g.fillStyle = 'rgba(16,20,28,.82)'; g.fill();
      g.lineWidth = 2; g.strokeStyle = econStatusColour(p.status); g.stroke();
      g.fillStyle = ECON_COL.power;                    // a small bolt
      g.beginPath();
      g.moveTo(sx + rad * .18, sy - rad * .62); g.lineTo(sx - rad * .38, sy + rad * .08); g.lineTo(sx - rad * .02, sy + rad * .08);
      g.lineTo(sx - rad * .2, sy + rad * .62); g.lineTo(sx + rad * .4, sy - rad * .1); g.lineTo(sx + rad * .04, sy - rad * .1);
      g.closePath(); g.fill();
      if (scale >= 0.9) {
        g.font = '600 10px "Segoe UI", system-ui, sans-serif'; g.textAlign = 'center';
        g.lineWidth = 3; g.strokeStyle = 'rgba(8,11,16,.85)'; g.fillStyle = '#f2f6fb';
        const label = n + ' fed' + (p.status < 100 ? '  ' + p.status + '%' : '');
        g.strokeText(label, sx, sy + rad + 11); g.fillText(label, sx, sy + rad + 11);
      }
    }
  }

  if (M.showProd) {                                    // output after damage and power: filled disc inside a dashed full-rate ring
    for (const rec of E.producers) {
      const [sx, sy] = toScreen(rec.o.x, rec.o.y);
      if (!on(sx, sy, 30)) continue;
      const col = ECON_COL[rec.kind], rf = Math.min(18, 2.5 + Math.sqrt(rec.full) * 0.55) * zoom,
            re = Math.min(18, 2.5 + Math.sqrt(rec.eff) * 0.55) * zoom;
      g.setLineDash([3, 3]); g.lineWidth = 1; g.strokeStyle = col; g.globalAlpha = 0.85;
      g.beginPath(); g.arc(sx, sy, rf, 0, Math.PI * 2); g.stroke(); g.setLineDash([]);
      if (rec.eff > 0) {
        g.globalAlpha = 0.55; g.fillStyle = col;
        g.beginPath(); g.arc(sx, sy, re, 0, Math.PI * 2); g.fill();
      }
      if (rec.eff < rec.full * 0.999) {                // something is lost: red tick
        g.globalAlpha = 1; g.fillStyle = econStatusColour(Math.round(100 * rec.eff / rec.full));
        g.beginPath(); g.arc(sx + rf, sy - rf, 3 * zoom, 0, Math.PI * 2); g.fill();
      }
      g.globalAlpha = 1;
    }
  }

  if (M.showSupply) {                                  // delivery sources, and the bridges deliveries cross
    for (const o of E.sources) {
      const [sx, sy] = toScreen(o.x, o.y);
      if (!on(sx, sy, 20)) continue;
      g.lineWidth = 1.6; g.strokeStyle = ECON_COL.source; g.globalAlpha = o.t === 8 ? 0.55 : 0.95;
      g.beginPath(); g.arc(sx, sy, (o.t === 8 ? 5 : 8) * zoom, 0, Math.PI * 2); g.stroke();
    }
    g.globalAlpha = 1;
    if (scale >= 0.5) for (const b of E.bridges) {
      if (b.status >= 100 && scale < 1.3) continue;
      const [sx, sy] = toScreen(b.x, b.y);
      if (!on(sx, sy, 12)) continue;
      const s = 3.5 * zoom;
      g.beginPath(); g.moveTo(sx, sy - s); g.lineTo(sx + s, sy); g.lineTo(sx, sy + s); g.lineTo(sx - s, sy); g.closePath();
      g.fillStyle = b.status >= 100 ? 'rgba(150,175,205,.55)' : econStatusColour(b.status);
      g.fill();
    }
  }
  g.restore();
}

// "makes 120/day: rate 300 x 40% damage x 100% power (Suwon Nuclear Power Plant, 22 km)" for the hover tip
function econTip(o, M) {
  if (!(M.showPower || M.showProd || M.showSupply)) return '';
  if (o._econ) {
    const r = o._econ;
    return (r.kind === 'fuel' ? 'fuel ' : 'supply+repl ') + Math.round(r.eff) + '/day (rate ' + r.full + ', damage ' + o.status +
           '%, power ' + (r.plant ? r.plant.status + '%' : 'assumed 100%, no plant of its own') + ')';
  }
  if (ECON_KIND.power.has(o.t)) return 'power plant ' + o.status + '%';
  if (ECON_KIND.source.has(o.t)) return 'supply source';
  if (o.t === 6) return 'bridge ' + o.status + '%';
  return '';
}

// the per-team totals box at the top right of the map
function econPanel(data, M, teamColour) {
  const E = econBuild(data);
  const box = document.createElement('div');
  const h = (tag, text, cls) => { const e = document.createElement(tag); if (cls) e.className = cls; e.textContent = text; return e; };
  const head = h('h5', 'Economy  (per day, after damage and power)  \u25be');
  head.title = 'click to fold the panel';
  head.onclick = () => { box.parentElement.classList.toggle('shut'); };
  box.appendChild(head);
  const owners = [...E.totals.keys()].filter(t => {
    const x = E.totals.get(t);
    return x.supply.n || x.fuel.n || x.plants;
  }).sort((a, b) => (E.totals.get(b).supply.full - E.totals.get(a).supply.full));
  for (const t of owners) {
    const x = E.totals.get(t), team = data.teams[t];
    const line = document.createElement('div'); line.className = 'econ-row';
    const sw = document.createElement('span'); sw.className = 'swatch'; sw.style.background = teamColour(t);
    line.appendChild(sw);
    line.appendChild(h('b', (team && team.name) || ('team ' + t)));
    const pct = (a, b) => b ? Math.round(100 * a / b) + '%' : '-';
    const bits = [];
    if (x.supply.n) bits.push('supply+repl ' + Math.round(x.supply.eff) + ' of ' + x.supply.full + ' (' + pct(x.supply.eff, x.supply.full) + ', ' + x.supply.n + ' sites' + (x.supply.assumed ? ', no plant of its own: power assumed 100%' : '') + ')');
    if (x.fuel.n) bits.push('fuel ' + Math.round(x.fuel.eff) + ' of ' + x.fuel.full + ' (' + x.fuel.n + ' refineries)');
    if (x.plants) bits.push(x.plants + ' power plants, mean ' + Math.round(x.plantSum / x.plants) + '%');
    if (x.bridges) bits.push(x.bridgesHit + ' of ' + x.bridges + ' bridges damaged');
    line.appendChild(h('div', bits.join('  ·  '), 'econ-sub'));
    box.appendChild(line);
  }
  box.appendChild(h('div', 'dashed ring = full rate, disc = after damage and power; line = producer to its plant; cyan ring = delivery source, diamond = bridge', 'econ-help'));
  return box;
}

// the objective pane: what this place does for the economy
function econDetail(e) {
  const g = document.createElement('div'); g.className = 'group';
  const h4 = document.createElement('h4'); h4.textContent = 'Economy'; g.appendChild(h4);
  const lines = [];
  if (e.kind) lines.push(e.type + ' feeds ' + e.kind + ': class rate ' + e.rate + ', damage ' + e.status + '% -> ' + Math.round(e.rate * e.status / 100) + '.');
  if (e.plant) lines.push('Nearest power plant: ' + e.plant.name + ' (' + e.plant.km + ' km) at ' + e.plant.status + '%. Output after power: ' + e.effective + ' per day.');
  else if (e.kind) lines.push('No power plant of this team in the file: the engine would use the nearest neutral or allied team\'s plant; shown at full power: ' + e.effective + ' per day.');
  if (e.power) lines.push('Power plant at ' + e.status + '%: every producer nearest to it is scaled by this.');
  if (e.source) lines.push('A supply delivery can start here (not while on the front line); damage does not matter.');
  for (const t of lines) {
    const d = document.createElement('div'); d.className = 'field-doc'; d.textContent = t; g.appendChild(d);
  }
  return g;
}
