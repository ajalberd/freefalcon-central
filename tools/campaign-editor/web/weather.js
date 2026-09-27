'use strict';

// Weather tab. The game's weather is one prevailing condition plus a field of
// moving fronts and random patches over it (src/graphics/include/
// weatherfronts.h). This edits both and shows the field on the map, at any
// time up to two days ahead, so a front can be aimed at a target for the hour
// a mission flies.
//
// The browser never computes the field: every change asks the server for a
// preview, and the server runs the same model the game does.

const CONDITION_NAMES = ['', 'Sunny', 'Fair', 'Poor', 'Inclement'];
const KIND_LABEL = {
  cold: 'Cold front', warm: 'Warm front', squall: 'Squall line',
  cell: 'Storm cell', high: 'Clearing',
};
const KIND_NOTE = {
  cold: 'a long band, steep ahead, rain dragging behind it; wind picks up, ' +
        'temperature drops',
  warm: 'a wide, shallow band whose overcast spreads far ahead of it',
  squall: 'a short, violent line of storms; lives a few hours',
  cell: 'a single round storm; lives a few hours',
  high: 'a patch of better weather moving through',
};

const WX = {file: null, data: null, weather: null, fronts: null, hours: 0,
            pick: -1, preview: null, img: null, playing: null, dirty: false};

async function weatherPanel(file) {
  const d = await api('/api/weather?' + qs({file: file}));
  if (WX.file !== file || !WX.dirty) {
    WX.file = file;
    WX.data = d;
    WX.weather = Object.assign({}, d.weather);
    WX.fronts = d.fronts ? JSON.parse(JSON.stringify(d.fronts)) : null;
    WX.preview = d.preview || null;
    WX.hours = 0;
    WX.pick = -1;
    WX.dirty = false;
  }
  if (WX.playing) { clearInterval(WX.playing); WX.playing = null; }

  const wrap = el('div', {class: 'pad wx'});
  const canvas = el('canvas', {class: 'wx-canvas'});
  const clock = el('span', {class: 'wx-clock'});
  const status = el('span', {class: 'hint'});
  const side = el('div', {class: 'wx-side'});

  // -- drawing ---------------------------------------------------------------

  if (!WX.img || WX.img.dataset.theater !== S.theater) {
    WX.img = new Image();
    WX.img.dataset.theater = S.theater;
    WX.img.src = '/api/map/image?' + qs({file: file});
  }
  WX.img.onload = () => draw();

  const sizeX = d.sizeKm[0], sizeY = d.sizeKm[1];

  function toPx(eastKm, northKm) {
    const s = canvas.width / sizeX;
    return [eastKm * s, (sizeY - northKm) * s];
  }

  // Clear air draws nothing. Fair is a light veil, Poor a grey overcast,
  // Inclement dark and blue -- one step each, blended across the fraction.
  const RAMP = [[1.3, 255, 255, 255, 0], [2.0, 250, 250, 250, 0.16],
                [2.5, 225, 228, 232, 0.28], [3.0, 175, 180, 188, 0.48],
                [3.5, 120, 128, 145, 0.6], [4.5, 55, 65, 95, 0.75]];

  function sevRGBA(v) {
    if (v <= RAMP[0][0]) return [255, 255, 255, 0];
    for (let i = 1; i < RAMP.length; i++) {
      const a = RAMP[i - 1], b = RAMP[i];
      if (v <= b[0] || i === RAMP.length - 1) {
        const t = Math.max(0, Math.min(1, (v - a[0]) / (b[0] - a[0])));
        return [1, 2, 3, 4].map(k => a[k] + (b[k] - a[k]) * t);
      }
    }
  }

  function sevColour(v) {
    const c = sevRGBA(v);
    return 'rgba(' + Math.round(c[0]) + ',' + Math.round(c[1]) + ',' +
      Math.round(c[2]) + ',' + c[3].toFixed(3) + ')';
  }

  function drawFront(g, f, i) {
    const s = canvas.width / sizeX;
    const [x, y] = toPx(f.eastKm, f.northKm);
    const h = f.headingDeg * Math.PI / 180;
    // Travel direction on screen: north is up, east is right.
    const dx = Math.sin(h), dy = -Math.cos(h);
    const strength = f.strength === undefined ? 1 : f.strength;
    const picked = i === WX.pick;
    const col = {cold: '#4da3ff', warm: '#ff6b5e', squall: '#c77dff',
                 cell: '#f2a33c', high: '#3fb950'}[f.kind] || '#fff';

    g.save();
    g.globalAlpha = 0.35 + 0.65 * strength;
    g.strokeStyle = col;
    g.fillStyle = col;
    g.lineWidth = picked ? 3.5 : 2;

    if (f.kind === 'cell' || f.kind === 'high') {
      g.beginPath();
      g.arc(x, y, Math.max(4, f.widthKm * s), 0, Math.PI * 2);
      g.setLineDash(f.kind === 'high' ? [6, 4] : []);
      g.stroke();
      g.setLineDash([]);
      g.font = 'bold 13px Segoe UI';
      g.textAlign = 'center';
      g.textBaseline = 'middle';
      g.fillText(f.kind === 'high' ? 'H' : '⚡', x, y);
    } else {
      // The surface line runs across the direction of travel.
      const L = f.lengthKm * s;
      const px = -dy, py = dx;
      g.beginPath();
      g.moveTo(x - px * L, y - py * L);
      g.lineTo(x + px * L, y + py * L);
      if (f.kind === 'squall') g.setLineDash([8, 5]);
      g.stroke();
      g.setLineDash([]);
      // Pips on the leading side: triangles cold, half-discs warm.
      const n = Math.max(2, Math.round(2 * L / 26));
      for (let k = 0; k < n; k++) {
        const t = -1 + (2 * k + 1) / n;
        const cx = x + px * L * t, cy = y + py * L * t;
        g.beginPath();
        if (f.kind === 'warm') {
          g.arc(cx, cy, 5, Math.atan2(py, px), Math.atan2(py, px) + Math.PI,
                false);
        } else {
          g.moveTo(cx - px * 5, cy - py * 5);
          g.lineTo(cx + dx * 8, cy + dy * 8);
          g.lineTo(cx + px * 5, cy + py * 5);
        }
        g.fill();
      }
    }

    // Where it is heading.
    g.beginPath();
    g.moveTo(x, y);
    g.lineTo(x + dx * 26, y + dy * 26);
    g.stroke();
    g.beginPath();
    g.moveTo(x + dx * 26, y + dy * 26);
    g.lineTo(x + dx * 19 - dy * 5, y + dy * 19 + dx * 5);
    g.lineTo(x + dx * 19 + dy * 5, y + dy * 19 - dx * 5);
    g.fill();

    if (picked) {
      g.globalAlpha = 1;
      g.strokeStyle = '#fff';
      g.lineWidth = 1;
      g.beginPath();
      g.arc(x, y, 7, 0, Math.PI * 2);
      g.stroke();
    }
    g.restore();
  }

  function drawWind(g) {
    // The prevailing wind, in the corner: an arrow blowing FROM its bearing.
    const h = (WX.weather.windFromDeg + 180) * Math.PI / 180;
    const dx = Math.sin(h), dy = -Math.cos(h);
    const x = 44, y = 44;
    g.save();
    g.fillStyle = 'rgba(10,13,18,.75)';
    g.beginPath();
    g.arc(x, y, 34, 0, Math.PI * 2);
    g.fill();
    g.strokeStyle = '#e6edf5';
    g.fillStyle = '#e6edf5';
    g.lineWidth = 2;
    g.beginPath();
    g.moveTo(x - dx * 22, y - dy * 22);
    g.lineTo(x + dx * 18, y + dy * 18);
    g.stroke();
    g.beginPath();
    g.moveTo(x + dx * 24, y + dy * 24);
    g.lineTo(x + dx * 12 - dy * 6, y + dy * 12 + dx * 6);
    g.lineTo(x + dx * 12 + dy * 6, y + dy * 12 - dx * 6);
    g.fill();
    g.font = '11px Segoe UI';
    g.textAlign = 'center';
    g.fillText(WX.weather.windFromDeg + '°/' + WX.weather.windKnots +
               'kt', x, y + 48);
    g.restore();
  }

  function draw() {
    const box = canvas.parentElement;
    const w = Math.max(320, Math.min(box ? box.clientWidth : 700, 900));
    canvas.width = w;
    canvas.height = Math.round(w * sizeY / sizeX);
    const g = canvas.getContext('2d');
    g.fillStyle = '#0a0d12';
    g.fillRect(0, 0, canvas.width, canvas.height);
    if (WX.img && WX.img.complete && WX.img.naturalWidth) {
      g.globalAlpha = 0.85;
      g.drawImage(WX.img, 0, 0, canvas.width, canvas.height);
      g.globalAlpha = 1;
    }
    const p = WX.preview;
    if (WX.fronts && p) {
      // One pixel per sample, scaled up with smoothing: the field is smooth,
      // and 16 km squares would say otherwise.
      const rows = p.grid.length, cols = p.grid[0].length;
      const off = document.createElement('canvas');
      off.width = cols;
      off.height = rows;
      const og = off.getContext('2d');
      const img = og.createImageData(cols, rows);
      p.grid.forEach((row, r) => row.forEach((v, c) => {
        const k = (r * cols + c) * 4, rgba = sevRGBA(v);
        img.data[k] = rgba[0];
        img.data[k + 1] = rgba[1];
        img.data[k + 2] = rgba[2];
        img.data[k + 3] = Math.round(rgba[3] * 255);
      }));
      og.putImageData(img, 0, 0);
      g.imageSmoothingEnabled = true;
      g.imageSmoothingQuality = 'high';
      g.drawImage(off, 0, 0, cols * p.stepKm * canvas.width / sizeX,
                  rows * p.stepKm * canvas.width / sizeX);
      p.items.forEach((f, i) => drawFront(g, f, i));
    } else {
      // No fronts: the whole map is one condition.
      g.fillStyle = sevColour(WX.weather.condition);
      g.fillRect(0, 0, canvas.width, canvas.height);
    }
    drawWind(g);
    clock.textContent = (p ? p.atText : d.nowText) +
      (WX.hours ? '   (+' + WX.hours + ' h)' : '   (campaign time now)');
  }

  // -- preview -----------------------------------------------------------------

  let previewSeq = 0;
  const refresh = guard(async () => {
    if (!WX.fronts) { WX.preview = null; draw(); return; }
    const seq = ++previewSeq;
    const p = await api('/api/weather/preview', {
      theater: S.theater, file: file, hours: WX.hours,
      weather: WX.weather, fronts: WX.fronts});
    if (seq !== previewSeq) return;
    WX.preview = p;
    draw();
  });

  const touched = () => {
    WX.dirty = true;
    status.textContent = 'unsaved changes';
    status.classList.add('warn');
  };

  // -- the time slider ---------------------------------------------------------

  const slider = el('input', {type: 'range', min: '0', max: '48', step: '1',
    value: String(WX.hours), class: 'wx-slider',
    oninput: e => { WX.hours = Number(e.target.value); refresh(); }});
  const play = el('button', {class: 'btn btn-sm', text: '▶ Play',
    onclick: () => {
      if (WX.playing) {
        clearInterval(WX.playing);
        WX.playing = null;
        play.textContent = '▶ Play';
        return;
      }
      play.textContent = '❚❚ Pause';
      WX.playing = setInterval(() => {
        if (!document.body.contains(canvas)) {
          clearInterval(WX.playing);
          WX.playing = null;
          return;
        }
        WX.hours = (WX.hours + 1) % 49;
        slider.value = String(WX.hours);
        refresh();
      }, 450);
    }});

  // Click the map to put the picked front's core there, as it is now.
  canvas.addEventListener('click', ev => {
    if (!WX.fronts || WX.pick < 0) return;
    const r = canvas.getBoundingClientRect();
    const s = canvas.width / sizeX;
    const east = (ev.clientX - r.left) * (canvas.width / r.width) / s;
    const north = sizeY - (ev.clientY - r.top) * (canvas.height / r.height) / s;
    const f = WX.fronts.items[WX.pick];
    // The click is where it should be at the previewed hour; move its
    // present position back along its track by as much.
    const h = f.headingDeg * Math.PI / 180;
    const km = f.speedKts * 1.852 * WX.hours;
    f.eastKm = Math.round((east - Math.sin(h) * km) * 10) / 10;
    f.northKm = Math.round((north - Math.cos(h) * km) * 10) / 10;
    touched();
    drawSide();
    refresh();
  });

  // -- the side panel -----------------------------------------------------------

  const num = (obj, key, attrs, after) => el('input', Object.assign({
    type: 'number', value: String(obj[key]),
    onchange: e => {
      const v = Number(e.target.value);
      if (!Number.isFinite(v)) return;
      obj[key] = v;
      e.target.classList.add('changed');
      touched();
      if (after) after();
      refresh();
    }}, attrs || {}));

  const field = (label, input, doc) => el('label', {class: 'field'}, [
    el('span', {text: label}), input,
    doc ? el('div', {class: 'field-doc', text: doc}) : null]);

  function conditionSelect() {
    const sel = el('select', {onchange: e => {
      const v = Number(e.target.value);
      WX.weather.condition = v;
      if (WX.fronts) WX.fronts.prevailing = v;
      e.target.classList.add('changed');
      touched();
      refresh();
    }});
    for (let c = 1; c <= 4; c++) {
      const o = el('option', {value: String(c), text: CONDITION_NAMES[c]});
      if (c === Math.round(WX.fronts ? WX.fronts.prevailing
                                     : WX.weather.condition)) o.selected = true;
      sel.appendChild(o);
    }
    return sel;
  }

  const addFront = guard(async (kind, count) => {
    const r = await api('/api/weather/generate', {
      theater: S.theater, file: file, weather: WX.weather,
      count: count || 1, kind: kind || null});
    if (!WX.fronts) {
      WX.fronts = {active: true, prevailing: WX.weather.condition,
                   noiseAmp: 0.8, seed: r.seed, items: []};
    }
    if (count && count > 1) WX.fronts.items = [];
    for (const f of r.items) {
      if (WX.fronts.items.length < 8) WX.fronts.items.push(f);
    }
    WX.pick = WX.fronts.items.length - 1;
    touched();
    drawSide();
    refresh();
  });

  function frontCard(f, i) {
    const picked = i === WX.pick;
    const kindSel = el('select', {onchange: e => {
      f.kind = e.target.value;
      // The game infers the kind from the shape, so the shape follows it.
      if (f.kind === 'cell' || f.kind === 'high') f.lengthKm = 0;
      else if (!f.lengthKm) f.lengthKm = f.kind === 'squall' ? 60 : 250;
      if (f.kind === 'high') f.severity = -Math.abs(f.severity || 2);
      else f.severity = Math.abs(f.severity || 2);
      if (f.kind === 'warm') f.tempDelta = Math.abs(f.tempDelta || 3);
      else if (f.kind !== 'high') f.tempDelta = -Math.abs(f.tempDelta || 3);
      if (f.kind === 'squall') f.widthKm = Math.min(f.widthKm, 18);
      else if (f.kind === 'cold' || f.kind === 'warm')
        f.widthKm = Math.max(f.widthKm, 22);
      touched();
      drawSide();
      refresh();
    }});
    for (const k of d.kinds) {
      const o = el('option', {value: k, text: KIND_LABEL[k]});
      if (k === f.kind) o.selected = true;
      kindSel.appendChild(o);
    }
    const band = f.kind !== 'cell' && f.kind !== 'high';
    const card = el('div', {class: 'wx-front' + (picked ? ' on' : '')}, [
      el('div', {class: 'wx-front-head', onclick: () => {
        WX.pick = picked ? -1 : i;
        drawSide();
        draw();
      }}, [
        el('span', {class: 'wx-dot wx-' + f.kind}),
        el('b', {text: KIND_LABEL[f.kind] || f.kind}),
        el('span', {class: 'hint', text: '  ' + Math.round(f.eastKm) + ', ' +
          Math.round(f.northKm) + ' km · ' + Math.round(f.headingDeg) +
          '° at ' + Math.round(f.speedKts) + ' kt'}),
        el('span', {class: 'spacer'}),
        el('button', {class: 'btn btn-sm', text: 'Remove',
          onclick: ev => {
            ev.stopPropagation();
            WX.fronts.items.splice(i, 1);
            WX.pick = -1;
            touched();
            drawSide();
            refresh();
          }}),
      ]),
    ]);
    if (!picked) return card;
    card.appendChild(el('p', {class: 'note', text:
      KIND_NOTE[f.kind] + '. Click the map to move it there' +
      (WX.hours ? ' at +' + WX.hours + ' h' : '') + '.'}));
    card.appendChild(el('div', {class: 'grid3'}, [
      field('Kind', kindSel),
      field('East, km', num(f, 'eastKm', {step: '1'})),
      field('North, km', num(f, 'northKm', {step: '1'})),
      field('Heading °', num(f, 'headingDeg', {step: '5', min: '0', max: '359'}),
            'direction it travels'),
      field('Speed, kt', num(f, 'speedKts', {step: '1', min: '0'})),
      field('Strength', num(f, 'severity', {step: '0.1', min: '-3', max: '3'}),
            f.kind === 'high' ? 'negative: steps better' : 'condition steps worse at the core'),
      field(band ? 'Depth, km' : 'Radius, km', num(f, 'widthKm', {step: '1', min: '1'}),
            band ? 'half the band’s depth' : null),
      band ? field('Length, km', num(f, 'lengthKm', {step: '10', min: '0'}),
                   'half the band’s length') : el('span'),
      field('Gusts, kt', num(f, 'windBoost', {step: '1', min: '0'}),
            'added to the wind at the core'),
      field('Temp Δ °C', num(f, 'tempDelta', {step: '1'})),
      field('Age, h', num(f, 'ageHours', {step: '0.5', min: '0'}),
            'it builds for the first 15% of its life'),
      field('Life, h', num(f, 'lifeHours', {step: '1', min: '1'}),
            'fades over the last 25%'),
    ]));
    return card;
  }

  function drawSide() {
    side.innerHTML = '';
    const w = WX.weather;

    side.appendChild(el('div', {class: 'panel'}, [
      el('header', {}, [el('h3', {text: 'Theater weather'})]),
      el('div', {class: 'panel-body'}, [
        el('div', {class: 'grid2'}, [
          field('Prevailing', conditionSelect(), WX.fronts
            ? 'what the map leans to; fronts and patches vary it'
            : 'one condition for the whole map'),
          field('Temperature °C', num(w, 'temperature', {step: '1'})),
          field('Wind from °', num(w, 'windFromDeg', {step: '5', min: '0', max: '359'}, draw)),
          field('Wind, kt', num(w, 'windKnots', {step: '1', min: '0'}, draw),
                'fronts and patches drift with it'),
          field('Cumulus base, ft', num(w, 'cumulusBase', {step: '500'})),
          field('Overcast base, ft', num(w, 'stratusBase', {step: '500'}),
                'Poor and Inclement move it with the fronts'),
          field('High layer, ft', num(w, 'stratus2Base', {step: '1000'})),
          field('Overcast depth, ×100 ft', num(w, 'overcastDepth', {step: '1'})),
          field('Contrails from, ×1000 ft', num(w, 'contrail', {step: '1'})),
        ]),
        d.isCampaign ? el('p', {class: 'note warn', text:
          'A campaign takes only the fronts, prevailing condition, wind and ' +
          'temperature from here; cloud bases come from the condition. With ' +
          'no fronts saved it uses the condition picked in Setup, as it ' +
          'always has.'}) : null,
      ]),
    ]));

    const fr = WX.fronts;
    const body = el('div', {class: 'panel-body'});
    side.appendChild(el('div', {class: 'panel'}, [
      el('header', {}, [
        el('h3', {text: 'Fronts'}),
        el('span', {class: 'hint', text: fr
          ? fr.items.length + ' of 8' : 'none saved in this file'}),
      ]),
      body,
    ]));

    if (!fr) {
      body.appendChild(el('p', {class: 'note', text:
        'This file carries no fronts, so the game makes up its own when it ' +
        'loads it (with g_nWeatherFronts 1, the default). Add some to decide ' +
        'the weather yourself: where it is, where it is going, and how bad.'}));
      body.appendChild(el('div', {class: 'wx-actions'}, [
        el('button', {class: 'btn btn-primary', text: 'Generate fronts',
          onclick: () => addFront(null, 3)}),
        el('button', {class: 'btn', text: 'Patches only, no fronts',
          onclick: () => {
            WX.fronts = {active: true, prevailing: w.condition, noiseAmp: 0.8,
                         seed: Math.floor(Math.random() * 2147483646) + 1,
                         items: []};
            touched();
            drawSide();
            refresh();
          }}),
      ]));
      return;
    }

    body.appendChild(el('div', {class: 'grid2'}, [
      field('Random patches', num(fr, 'noiseAmp', {step: '0.1', min: '0', max: '2'}),
            '0 none · 1 about a third of the map a step off prevailing'),
      el('label', {class: 'field'}, [
        el('span', {text: 'Pattern'}),
        el('button', {class: 'btn btn-sm', text: 'Reroll the patches',
          onclick: () => {
            fr.seed = Math.floor(Math.random() * 2147483646) + 1;
            touched();
            refresh();
          }}),
      ]),
    ]));
    body.appendChild(el('label', {class: 'chk'}, [
      el('input', {type: 'checkbox', checked: fr.active, onchange: e => {
        fr.active = e.target.checked;
        touched();
        refresh();
      }}),
      el('span', {text: 'Varying weather on (off: prevailing everywhere)'}),
    ]));

    fr.items.forEach((f, i) => body.appendChild(frontCard(f, i)));

    const kindPick = el('select');
    for (const k of d.kinds) {
      kindPick.appendChild(el('option', {value: k, text: KIND_LABEL[k]}));
    }
    body.appendChild(el('div', {class: 'wx-actions'}, [
      kindPick,
      el('button', {class: 'btn', text: 'Add', disabled: fr.items.length >= 8,
        onclick: () => addFront(kindPick.value, 1)}),
      el('span', {class: 'spacer'}),
      el('button', {class: 'btn', text: 'Regenerate all',
        onclick: () => addFront(null, 3)}),
      el('button', {class: 'btn', text: 'Remove all', onclick: () => {
        WX.fronts = null;
        WX.pick = -1;
        touched();
        drawSide();
        refresh();
      }}),
    ]));
  }

  const save = guard(async () => {
    await api('/api/weather/edit', {theater: S.theater, file: file,
      weather: WX.weather, fronts: WX.fronts});
    WX.dirty = false;
    status.textContent = 'staged — Save writes the file';
    status.classList.remove('warn');
    await refreshPending();
  });

  const revert = guard(async () => {
    WX.dirty = false;
    await drawView();
  });

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: 'Weather — ' + file}),
      status,
      el('span', {class: 'spacer'}),
      el('button', {class: 'btn btn-sm', text: 'Revert', onclick: revert}),
      el('button', {class: 'btn btn-sm btn-primary', text: 'Apply',
                    onclick: save}),
    ]),
    el('div', {class: 'panel-body wx-body'}, [
      el('div', {class: 'wx-mapcol'}, [
        el('div', {class: 'wx-canvas-box'}, canvas),
        el('div', {class: 'wx-time'}, [play, slider, clock]),
        el('div', {class: 'wx-legend'}, [
          el('span', {class: 'wx-swatch', style: 'background:' + sevColour(2.0)}), 'Fair ',
          el('span', {class: 'wx-swatch', style: 'background:' + sevColour(3.0)}), 'Poor ',
          el('span', {class: 'wx-swatch', style: 'background:' + sevColour(4.0)}), 'Inclement ',
          el('span', {class: 'wx-dot wx-cold'}), 'cold ',
          el('span', {class: 'wx-dot wx-warm'}), 'warm ',
          el('span', {class: 'wx-dot wx-squall'}), 'squall ',
          el('span', {class: 'wx-dot wx-cell'}), 'storm ',
          el('span', {class: 'wx-dot wx-high'}), 'clearing',
        ]),
      ]),
      side,
    ]),
  ]));

  if (WX.dirty) {
    status.textContent = 'unsaved changes';
    status.classList.add('warn');
  }

  drawSide();
  setTimeout(() => { draw(); if (WX.fronts && WX.hours) refresh(); }, 0);
  return wrap;
}
