'use strict';

// Aircraft tab: what one jet can hang on each hardpoint, from its vehicle
// class (FALCON4.VCD) -- the same lists the loadout screen offers. Search by
// aircraft, or by weapon to see every aircraft that can carry it.

const AC = {data: null, theater: null, file: null, pick: null, q: '', wq: '',
            flyingOnly: false};

async function aircraftPanel() {
  if (!AC.data || AC.theater !== S.theater || AC.file !== S.file) {
    AC.data = await api('/api/aircraft?' + qs());
    if (AC.theater !== S.theater) AC.pick = null;
    AC.theater = S.theater;
    AC.file = S.file;
  }
  try { AC.flyingOnly = localStorage.getItem('ffcamp.aircraft.flying') === '1'; }
  catch (e) { /* no storage */ }
  const all = AC.data.aircraft;
  const wrap = el('div', {class: 'pad'});
  const list = el('div', {class: 'ac-list'});
  const detail = el('div');

  const carries = (a, needle) => a.weapons.some(w =>
    w.name.toLowerCase().includes(needle));

  function drawList() {
    list.textContent = '';
    const q = AC.q.trim().toLowerCase(), wq = AC.wq.trim().toLowerCase();
    // In service first, most airframes first; the rest alphabetical after.
    const shown = all.filter(a => (!q || a.name.toLowerCase().includes(q)) &&
                                  (!wq || carries(a, wq)) &&
                                  (!AC.flyingOnly || a.squadrons))
      .sort((a, b) => (b.inService - a.inService) ||
                      a.name.localeCompare(b.name));
    for (const a of shown) {
      const hit = wq ? a.weapons.filter(w => w.name.toLowerCase().includes(wq))
                         .map(w => w.name + ' ×' + w.max).join(', ') : '';
      list.appendChild(el('div', {
        class: 'ac-item' + (AC.pick === a.vehicle ? ' on' : ''),
        onclick: () => { AC.pick = a.vehicle; drawList(); drawDetail(); },
      }, [el('span', {class: a.squadrons ? '' : 'ac-idle', text: a.name}),
          el('span', {class: 'hint', text: hit || (a.squadrons
            ? a.inService + ' in ' + a.squadrons + ' sqn · ' +
              Object.keys(a.teams).join(', ')
            : a.weapons.length + ' weapons')})]));
    }
    if (!shown.length) {
      list.appendChild(el('div', {class: 'ac-item', text: 'Nothing matches.'}));
    }
    if (!AC.pick || !shown.some(a => a.vehicle === AC.pick)) {
      AC.pick = shown.length ? shown[0].vehicle : null;
      if (list.firstChild && shown.length) list.firstChild.classList.add('on');
      drawDetail();
    }
  }

  function drawDetail() {
    detail.textContent = '';
    const a = all.find(x => x.vehicle === AC.pick);
    if (!a) return;
    const wq = AC.wq.trim().toLowerCase();
    const hps = a.hardpoints;
    detail.appendChild(el('div', {class: 'panel'}, [
      el('header', {}, [
        el('h3', {text: a.name}),
        el('span', {class: 'hint', text: hps.length + ' stations · ' +
          a.weapons.length + ' weapons · class ' + a.vehicle}),
      ]),
      el('div', {class: 'panel-body'}, [
        el('p', {class: a.squadrons ? 'ac-service' : 'note', text: a.squadrons
          ? 'In ' + (AC.data.file || 'this campaign') + ': ' + a.inService +
            ' aircraft in ' + a.squadrons + ' squadron' +
            (a.squadrons === 1 ? '' : 's') + ' — ' +
            Object.entries(a.teams).map(([t, v]) => t + ' ' + v.aircraft +
              ' in ' + v.squadrons).join(', ') + '.'
          : 'No squadron flies this in ' + (AC.data.file || 'this campaign') +
            '. Same-named aircraft are separate classes (another country’s ' +
            'version, or a spare) — the class number tells them apart.'}),
        el('p', {class: 'note', text:
          'Per station, what the loadout screen will offer and how many fit ' +
          'there. “Most per jet” below adds up the best each ' +
          'station can take, so it is a ceiling: stations also share weight ' +
          'and drag limits the list does not show.'}),
        el('table', {class: 'data ac-hp'}, [
          el('thead', {}, el('tr', {}, ['Station', 'Can carry'].map(h =>
            el('th', {text: h})))),
          el('tbody', {}, hps.map(h => el('tr', {}, [
            el('td', {class: 'num', text: h.gun ? 'gun' : String(h.hardpoint)}),
            el('td', {}, el('div', {class: 'ac-opts'}, h.options.map(o =>
              el('span', {class: 'ac-opt' + (wq && o.name.toLowerCase()
                                               .includes(wq) ? ' hit' : ''),
                          text: o.name + (h.gun ? '' : ' ×' + o.qty)})))),
          ]))),
        ]),
        el('h4', {text: 'Most per jet', style: 'margin:16px 0 6px'}),
        el('div', {class: 'ac-opts'}, a.weapons.filter(w =>
          !a.hardpoints.some(h => h.gun && h.options.some(o =>
            o.index === w.index))).map(w =>
          el('span', {class: 'ac-opt' + (wq && w.name.toLowerCase()
                                           .includes(wq) ? ' hit' : ''),
                      title: 'stations ' + w.hardpoints.join(', '),
                      text: w.name + ' ×' + w.max}))),
      ]),
    ]));
  }

  const search = el('input', {type: 'text', value: AC.q, spellcheck: 'false',
    placeholder: 'Aircraft… (A-10, F-16)',
    oninput: e => { AC.q = e.target.value; drawList(); }});
  const wsearch = el('input', {type: 'text', value: AC.wq, spellcheck: 'false',
    placeholder: 'Carries weapon… (CBU-87, AGM-65)',
    oninput: e => { AC.wq = e.target.value; drawList(); drawDetail(); }});

  const flyingBox = el('label', {class: 'chk'}, [
    el('input', {type: 'checkbox', checked: AC.flyingOnly, onchange: e => {
      AC.flyingOnly = e.target.checked;
      try { localStorage.setItem('ffcamp.aircraft.flying',
                                 AC.flyingOnly ? '1' : '0'); }
      catch (err) { /* ignore */ }
      drawList();
    }}),
    el('span', {text: 'Only aircraft flying in this campaign'}),
  ]);
  const total = all.reduce((n, a) => n + a.inService, 0);
  const sqns = all.reduce((n, a) => n + a.squadrons, 0);

  wrap.appendChild(el('div', {class: 'ac-search'}, [search, wsearch]));
  wrap.appendChild(el('div', {class: 'ac-bar'}, [
    flyingBox,
    el('span', {class: 'hint', text: AC.data.file
      ? total.toLocaleString() + ' airframes in ' + sqns + ' squadrons in ' +
        AC.data.file + ' — every aircraft on every squadron’s ' +
        'roster, tankers and transports included. The Progress tab’s ' +
        'aircraft count is the game’s own, which counts only combat ' +
        'squadrons and discounts each by its airbase’s damage.'
      : 'Open a campaign to see what is flying in it.'}),
    AC.data.flyingError ? el('span', {class: 'hint warn', text:
      'unit list unreadable: ' + AC.data.flyingError}) : null,
  ]));
  wrap.appendChild(el('div', {class: 'ac-wrap'}, [list, detail]));
  drawList();
  drawDetail();
  return wrap;
}
