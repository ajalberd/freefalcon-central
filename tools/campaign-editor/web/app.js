'use strict';

// The editor works the way Mission Commander does: you open a campaign file
// first, and every tab after that is a view of *that file*. The one exception
// is Database, which edits the theater's shared CampaignDB and so applies to
// every campaign in the theater -- the UI says so where it matters.

const S = {
  gamedir: '',
  theaters: [],
  theater: null,     // selected .tdf path
  info: null,        // /api/theater payload for the selection
  file: null,        // the open .cam / .tac
  tab: null,         // 'theater' | 'map' | ... | 'db:<table>'
  rowIndex: null,
  pending: [],
};

const $ = (sel, root) => (root || document).querySelector(sel);
const el = (tag, attrs, kids) => {
  const n = document.createElement(tag);
  for (const k in (attrs || {})) {
    const v = attrs[k];
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
    else if (v === true) n.setAttribute(k, '');
    else n.setAttribute(k, v);
  }
  for (const kid of [].concat(kids || [])) {
    if (kid === null || kid === undefined || kid === false) continue;
    n.appendChild(typeof kid === 'string' ? document.createTextNode(kid) : kid);
  }
  return n;
};

// --- transport --------------------------------------------------------------

async function api(path, body) {
  const opts = body === undefined ? {} : {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body),
  };
  const res = await fetch(path, opts);
  let data;
  try { data = await res.json(); }
  catch (e) { throw new Error('server returned ' + res.status); }
  if (!res.ok || data.error) throw new Error(data.error || ('HTTP ' + res.status));
  return data;
}

function toast(msg, kind) {
  const n = el('div', {class: 'toast ' + (kind || 'info'), text: msg});
  $('#toasts').appendChild(n);
  setTimeout(() => n.remove(), kind === 'err' ? 8000 : 4200);
}

const guard = fn => (...args) => fn(...args).catch(e => toast(e.message, 'err'));

const qs = (extra) => {
  const p = new URLSearchParams({theater: S.theater});
  if (S.file) p.set('file', S.file);
  for (const k in (extra || {})) p.set(k, extra[k]);
  return p.toString();
};

// --- shell ------------------------------------------------------------------

async function loadState() {
  const st = await api('/api/state');
  S.gamedir = st.gamedir;
  S.theaters = st.theaters;
  S.pending = st.pending;
  const note = $('#gamedir-note');
  if (note) note.hidden = st.gamedir_ok;
  $('#gamedir-input').value = st.gamedir;
  drawRail();
  drawPending();
  drawHeader();
}

function drawPending() {
  const n = S.pending.length;
  const pill = $('#pending');
  pill.textContent = n ? (n + (n === 1 ? ' unsaved change' : ' unsaved changes'))
                       : 'no changes';
  pill.className = 'pill ' + (n ? 'pill-warn' : 'pill-quiet');
  $('#btn-save').disabled = !n;
  const d = $('#btn-discard');
  if (d) d.disabled = !n;
}

// --- left rail: theaters, and the campaigns inside the open one -------------

function drawRail() {
  const list = $('#theater-list');
  list.textContent = '';

  const dirtyCamps = new Set(
    S.pending.filter(p => p.kind === 'campaign').map(p => p.name));

  for (const t of S.theaters) {
    const open = t.file === S.theater;
    const li = el('li', {});
    li.appendChild(el('button', {
      class: 'theater' + (open ? ' on' : ''),
      onclick: guard(() => selectTheater(t.file)),
    }, [
      el('span', {class: 'tname', text: t.name}),
      el('span', {class: 'tmeta',
        text: t.campaigndir + (t.has_campaigndb ? '' : '  · no CampaignDB')}),
    ]));

    if (open && S.info) {
      const files = el('ul', {class: 'camp-list'});
      for (const c of S.info.campaigns) {
        files.appendChild(el('li', {}, el('button', {
          class: 'camp' + (S.file === c.file ? ' on' : ''),
          onclick: guard(() => openCampaign(c.file)),
        }, [
          el('span', {class: 'cname', text: c.file}),
          dirtyCamps.has(c.file) ? el('span', {class: 'dot'}) : null,
          el('span', {class: 'csize',
            text: (c.bytes / 1024).toFixed(0) + ' kB'}),
        ])));
      }
      files.appendChild(el('li', {}, el('button', {
        class: 'camp new',
        onclick: () => openNewCampaign(),
      }, '+ New campaign…')));
      li.appendChild(files);
    }
    list.appendChild(li);
  }
}

function drawHeader() {
  const strip = $('#filebar');
  strip.textContent = '';
  if (!S.info) { strip.hidden = true; return; }
  strip.hidden = false;

  const row = (label, value, cls) => {
    strip.appendChild(el('span', {class: 'fb-label', text: label}));
    strip.appendChild(el('span', {class: 'fb-value mono' + (cls ? ' ' + cls : ''),
                                  text: value}));
  };
  row('File name', S.file
      ? S.gamedir + '\\' + S.info.campaign_dir + '\\' + S.file
      : 'no campaign open — pick one on the left',
      S.file ? '' : 'faint');
  row('Theater', S.info.name + '   ·   ' + S.info.tdf);
  row('Database', S.gamedir + '\\' + S.info.db_dir);
}

async function selectTheater(tdf) {
  S.theater = tdf;
  S.info = await api('/api/theater?theater=' + encodeURIComponent(tdf));
  S.file = null;
  S.tab = 'theater';
  S.rowIndex = null;
  if (typeof mapReset === 'function') mapReset();
  drawRail();
  drawHeader();
  drawTabs();
  await drawView();
}

async function openCampaign(file) {
  S.file = file;
  S.rowIndex = null;
  if (typeof mapReset === 'function') mapReset();
  if (!S.tab || S.tab === 'theater' || S.tab.startsWith('db:')) S.tab = 'map';
  drawRail();
  drawHeader();
  drawTabs();
  await drawView();
}

// --- tabs -------------------------------------------------------------------

// Scoped to the open campaign file, except Database, which is the theater's.
const FILE_TABS = [
  {id: 'map', label: 'Map'},
  {id: 'campaign', label: 'Campaign'},
  {id: 'teams', label: 'Teams'},
  {id: 'units', label: 'Units'},
  {id: 'objectives', label: 'Objectives'},
  {id: 'squadrons', label: 'Squadrons'},
  {id: 'victory', label: 'Victory'},
  {id: 'weather', label: 'Weather'},
  {id: 'progress', label: 'Progress'},
  {id: 'aircraft', label: 'Aircraft'},
  {id: 'members', label: 'File contents'},
];

// CampaignDB tables, grouped so the sub-bar is readable rather than 19 wide.
const TABLE_GROUPS = [
  ['Order of battle', ['unit', 'vehicle', 'squadstores']],
  ['Weapons', ['weapon', 'weaponlist', 'simweapon', 'rocket']],
  ['Places', ['objective', 'feature', 'featureentry', 'ptheader', 'ptdata']],
  ['Sensors', ['radar', 'rwr', 'irst', 'visual', 'simacdef']],
  ['Low level', ['class', 'dirtydata']],
];

const go = guard(async (tab) => {
  S.tab = tab;
  S.rowIndex = null;
  drawTabs();
  await drawView();
});

function drawTabs() {
  const tabs = $('#tabs');
  tabs.textContent = '';
  tabs.hidden = !S.info;
  if (!S.info) return;

  const dirtyTables = new Set(
    S.info.pending.filter(p => p.kind === 'table').map(p => p.name));
  const dirtyCamps = new Set(
    S.info.pending.filter(p => p.kind === 'campaign').map(p => p.name));
  const fileDirty = S.file && dirtyCamps.has(S.file);

  const row1 = el('div', {class: 'tab-row'});
  row1.appendChild(el('button', {
    class: S.tab === 'theater' ? 'on' : '',
    onclick: () => go('theater'),
  }, 'Theater'));

  for (const t of FILE_TABS) {
    row1.appendChild(el('button', {
      class: (S.tab === t.id ? 'on' : '') + (S.file ? '' : ' off'),
      title: S.file ? '' : 'Open a campaign file first',
      onclick: () => {
        if (!S.file) { toast('Open a campaign file first', 'info'); return; }
        go(t.id);
      },
    }, [t.label,
        (fileDirty && t.id !== 'members') ? el('span', {class: 'dot'}) : null]));
  }

  row1.appendChild(el('span', {class: 'tab-gap'}));
  row1.appendChild(el('button', {
    class: (S.tab || '').startsWith('db:') ? 'on' : '',
    onclick: () => go('db:' + firstTable()),
  }, ['Database', dirtyTables.size ? el('span', {class: 'dot'}) : null]));
  tabs.appendChild(row1);

  if ((S.tab || '').startsWith('db:')) {
    const row2 = el('div', {class: 'tab-row sub'});
    const present = new Map(S.info.tables.map(t => [t.name, t]));
    const known = new Set();
    const groups = TABLE_GROUPS.map(([label, names]) => {
      const items = names.filter(n => present.has(n));
      items.forEach(n => known.add(n));
      return [label, items];
    });
    const rest = S.info.tables.map(t => t.name).filter(n => !known.has(n));
    if (rest.length) groups.push(['More', rest]);

    for (const [label, names] of groups) {
      if (!names.length) continue;
      const box = el('div', {class: 'tab-group'});
      box.appendChild(el('span', {class: 'lbl', text: label}));
      for (const n of names) {
        const t = present.get(n);
        box.appendChild(el('button', {
          class: S.tab === 'db:' + n ? 'on' : '',
          onclick: () => go('db:' + n),
        }, [
          t.title,
          el('span', {class: 'count', text: String(t.rows)}),
          dirtyTables.has(n) ? el('span', {class: 'dot'}) : null,
        ]));
      }
      row2.appendChild(box);
    }
    tabs.appendChild(row2);
  }
}

function firstTable() {
  for (const [, names] of TABLE_GROUPS) {
    for (const n of names) {
      if (S.info.tables.some(t => t.name === n)) return n;
    }
  }
  const t = S.info.tables[0];
  return t ? t.name : '';
}

// Jump to a database table with a search already filled in. Used by the map's
// detail panes ("Edit this unit type").
const openTable = (...a) => guard(openTableImpl)(...a);

async function openTableImpl(table, query) {
  S.tab = 'db:' + table;
  S.rowIndex = null;
  tablePanel.q = query || '';
  drawTabs();
  await drawView();
}

async function drawView() {
  const view = $('#view');
  view.textContent = '';
  if (!S.info) {
    view.appendChild(el('div', {class: 'empty'}, [
      el('h2', {text: 'Pick a theater to begin'}),
      el('p', {text: 'Then open one of its campaign files. Every tab after ' +
                     'that edits that file; the Database tab edits the ' +
                     'theater’s shared tables.'}),
    ]));
    return;
  }
  if (!S.tab) S.tab = 'theater';

  let node = null;
  if (S.tab === 'theater') node = theaterPanel();
  else if (S.tab.startsWith('db:')) node = await tablePanel(S.tab.slice(3));
  else if (!S.file) {
    view.appendChild(el('div', {class: 'empty'}, [
      el('h2', {text: 'No campaign open'}),
      el('p', {text: 'Pick a .cam or .tac on the left.'}),
    ]));
    return;
  }
  else if (S.tab === 'map') node = await mapPanel(S.file);
  else if (S.tab === 'campaign') node = await campaignPanel(S.file);
  else if (S.tab === 'teams') node = await teamsPanel(S.file);
  else if (S.tab === 'units') node = await unitsPanel(S.file);
  else if (S.tab === 'objectives') node = await objectivesPanel(S.file);
  else if (S.tab === 'squadrons') node = await squadronsPanel(S.file);
  else if (S.tab === 'victory') node = await victoryPanel(S.file);
  else if (S.tab === 'weather') node = await weatherPanel(S.file);
  else if (S.tab === 'progress') node = await progressPanel(S.file);
  else if (S.tab === 'aircraft') node = await aircraftPanel();
  else if (S.tab === 'members') node = await membersPanel(S.file);

  if (!node) return;
  view.appendChild(node);
  // Canvas panels can only size themselves once they are in the document.
  // Call it straight away -- getBoundingClientRect forces the layout we need,
  // and a requestAnimationFrame here would never fire in a backgrounded tab.
  if (node._afterAttach) {
    node._afterAttach();
    requestAnimationFrame(() => node._afterAttach());
  }
}

// --- theater panel ----------------------------------------------------------

function theaterPanel() {
  const i = S.info;
  const wrap = el('div', {class: 'pad'});

  const dl = el('dl', {class: 'kv'});
  const add = (k, v) => {
    dl.appendChild(el('dt', {text: k}));
    dl.appendChild(el('dd', {class: 'mono', text: String(v)}));
  };
  add('Theater file', i.tdf);
  add('Campaign directory', i.campaign_dir);
  add('CampaignDB', i.db_dir);
  for (const k of Object.keys(i.values)) {
    if (['name', 'desc', 'campaigndir', 'specialdbdir'].includes(k)) continue;
    add(k, i.values[k]);
  }

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [el('h3', {text: i.name}),
                      el('span', {class: 'hint', text: i.desc || ''})]),
    el('div', {class: 'panel-body'}, dl),
  ]));

  const rows = i.campaigns.map(c => el('tr', {
    class: c.file === S.file ? 'on' : '',
    onclick: guard(() => openCampaign(c.file)),
  }, [
    el('td', {text: c.file}),
    el('td', {text: c.kind}),
    el('td', {class: 'num', text: c.bytes.toLocaleString()}),
  ]));
  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: 'Campaigns in this theater'}),
      el('button', {class: 'btn btn-sm', onclick: () => openNewCampaign()},
         'New campaign…'),
    ]),
    el('table', {class: 'data'}, [
      el('thead', {}, el('tr', {}, ['File', 'Kind', 'Bytes']
        .map(h => el('th', {text: h})))),
      el('tbody', {}, rows),
    ]),
  ]));

  const shipped = ['korea.tdf', 'korea_2012', 'eurowar']
    .some(n => i.tdf.toLowerCase().includes(n));

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: 'Make a new theater from this one'}),
      el('span', {class: 'hint',
        text: 'copies the campaign directory, writes a .tdf, adds it to theater.lst'}),
    ]),
    el('div', {class: 'panel-body'}, [
      el('p', {class: 'note', text:
        'The copy gets its own CampaignDB, so its class table, unit tables ' +
        'and weapon tables are independent of this one. Terrain, objects and ' +
        'misc textures keep pointing at the same folders, which is what makes ' +
        'an era variant cheap: same map, different order of battle.'}),
      el('button', {class: 'btn btn-primary',
        onclick: () => openNewTheater(i.tdf)}, 'Clone "' + i.name + '"'),
      shipped ? null : el('button', {class: 'btn btn-danger',
        style: 'margin-left:8px',
        onclick: guard(() => deleteTheater(i.tdf, i.name))},
        'Delete this theater'),
    ]),
  ]));

  return wrap;
}

// --- shared helpers for the file panels -------------------------------------

function campaignData(file) {
  return api('/api/campaign?' + qs({file: file}));
}

function pushCampaign(file, fields, teams) {
  return api('/api/campaign/edit', Object.assign(
    {theater: S.theater, file: file},
    fields ? {fields: fields} : {},
    teams ? {teams: teams} : {}));
}

const MS_DAY = 86400000, MS_HR = 3600000, MS_MIN = 60000;

function durationHint(ms) {
  if (!ms) return '';
  const d = Math.floor(ms / MS_DAY), h = Math.floor(ms % MS_DAY / MS_HR);
  const m = Math.floor(ms % MS_HR / MS_MIN);
  return [d ? d + 'd' : '', h ? h + 'h' : '', m ? m + 'm' : '']
    .filter(Boolean).join(' ') || '0m';
}

function fieldEditor(c, file, changed, key) {
  const val = c.fields[key];
  const isText = typeof val === 'string';
  const push = guard(async () => {
    if (!Object.keys(changed).length) return;
    await pushCampaign(file, changed);
    for (const k in changed) delete changed[k];
    await refreshPending();
  });
  const input = el('input', {
    type: isText ? 'text' : 'number',
    value: String(val),
    spellcheck: 'false',
    oninput: e => {
      changed[key] = isText ? e.target.value : Number(e.target.value);
      e.target.classList.add('changed');
    },
    onchange: push,
  });
  const doc = c.docs[key];
  const extra = /Time|TimeLimit/.test(key) && !isText ? durationHint(val) : '';
  return el('label', {class: 'field'}, [
    el('span', {text: key}),
    input,
    (doc || extra)
      ? el('div', {class: 'field-doc',
          text: [doc, extra && '= ' + extra].filter(Boolean).join('   ')})
      : null,
  ]);
}

// --- campaign header --------------------------------------------------------

async function campaignPanel(file) {
  const c = await campaignData(file);
  const wrap = el('div', {class: 'pad'});
  const changed = {};
  const F = k => fieldEditor(c, file, changed, k);

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: file}),
      el('span', {class: 'hint', text: 'data version ' + c.version +
        '   ·   ' + c.squadronCount + ' squadrons   ·   ' +
        (c.events[0] + c.events[1]) + ' logged events'}),
    ]),
    el('div', {class: 'panel-body'}, [
      el('p', {class: 'note', text:
        'The campaign-level knobs the engine reads on load. Fields it ' +
        'recomputes anyway — tasking times, the occupation map, per-unit ' +
        'state — round-trip untouched rather than being exposed here.'}),
      el('div', {class: 'grid3'}, ['UIName', 'Scenario', 'TheaterName'].map(F)),
      el('div', {class: 'grid4'},
        ['CurrentTime', 'CurrentDay', 'DayZero', 'Tempo'].map(F)),
      el('div', {class: 'grid4'},
        ['ActiveTeams', 'EndgameResult', 'Situation', 'Brief'].map(F)),
    ]),
  ]));

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: 'Starting balance'}),
      el('span', {class: 'hint',
        text: 'friendly vs enemy strength at day zero'}),
    ]),
    el('div', {class: 'panel-body'},
      el('div', {class: 'grid4'},
        ['GroundRatio', 'AirRatio', 'AirDefenseRatio', 'NavalRatio'].map(F))),
  ]));

  return wrap;
}

// --- victory conditions -----------------------------------------------------

// Every edit here rewrites one line of the .tri and nothing else, so the script
// keeps its comments and layout. Redrawing afterwards re-reads the file, which
// is also how the English descriptions stay honest.
async function editTrigger(file, line, fields) {
  await api('/api/triggers/edit',
            {theater: S.theater, file: file, line: line, fields: fields});
  await refreshPending();
  await drawView();
}

function teamSelect(tri, value, onchange) {
  const sel = el('select', {onchange: guard(async ev =>
    onchange(Number(ev.target.value)))});
  for (const t of tri.teams) {
    if (!t.name && t.index) continue;            // unused team slots
    const o = el('option', {value: String(t.index),
                            text: t.name || ('team ' + t.index)});
    if (t.index === value) o.selected = true;
    sel.appendChild(o);
  }
  if (!tri.teams.some(t => t.index === value && (t.name || !t.index))) {
    const o = el('option', {value: String(value), text: 'team ' + value});
    o.selected = true;
    sel.appendChild(o);
  }
  return sel;
}

function endgameCard(file, tri, e) {
  const card = el('div', {class: 'endgame'});
  card.appendChild(el('div', {class: 'eg-head'}, [
    el('span', {class: 'eg-result', text: 'result ' + e.result}),
    el('span', {class: 'eg-note', text: e.comment || ''}),
  ]));

  if (!e.editable || !e.editable.length) {
    for (const cond of e.conditions) {
      card.appendChild(el('div', {class: 'eg-cond'}, [
        el('span', {class: 'eg-when', text: 'when'}),
        el('span', {text: cond}),
      ]));
    }
    return card;
  }

  for (const ed of e.editable) {
    const ctl = el('div', {class: 'eg-ctl'});

    if (ed.verb === 'IF_CONTROLLED') {
      ctl.appendChild(teamSelect(tri, ed.team, v =>
        editTrigger(file, ed.line, {team: v})));
      const mode = el('select', {onchange: guard(async ev =>
        editTrigger(file, ed.line, {mode: ev.target.value}))});
      for (const pair of [['A', 'controls all of'], ['O', 'controls any of']]) {
        const o = el('option', {value: pair[0], text: pair[1]});
        if (pair[0] === ed.mode) o.selected = true;
        mode.appendChild(o);
      }
      ctl.appendChild(mode);
      ctl.appendChild(objectiveList(file, ed));
    } else if (ed.verb === 'IF_CAMPAIGN_DAY') {
      const cmp = el('select', {onchange: guard(async ev =>
        editTrigger(file, ed.line, {cmp: ev.target.value}))});
      for (const pair of [['G', 'the campaign day is at least'],
                          ['L', 'the campaign day is under']]) {
        const o = el('option', {value: pair[0], text: pair[1]});
        if (pair[0] === ed.cmp) o.selected = true;
        cmp.appendChild(o);
      }
      ctl.appendChild(cmp);
      ctl.appendChild(numberBox(file, ed, ed.value, 1, 365, 'days'));
    } else if (ed.verb === 'IF_BORDOM_HOURS') {
      ctl.appendChild(el('span', {text: 'nothing significant happens for'}));
      ctl.appendChild(numberBox(file, ed, ed.value, 0, 10000, 'hours'));
      ctl.appendChild(el('span', {class: 'hint',
        text: '= ' + (ed.value / 24).toFixed(1) + ' days'}));
    } else if (ed.verb === 'END_GAME') {
      ctl.appendChild(el('span', {text: 'end the campaign, result code'}));
      ctl.appendChild(numberBox(file, ed, ed.value, 0, 255, ''));
      ctl.appendChild(el('span', {class: 'hint',
        text: 'this is what lands in EndgameResult'}));
    }

    card.appendChild(el('div', {class: 'eg-edit'}, [
      el('span', {class: 'eg-when',
        text: ed.verb === 'END_GAME' ? 'then' : 'when'}),
      ctl,
    ]));
  }
  return card;
}

function numberBox(file, ed, value, min, max, suffix) {
  const box = el('span', {class: 'eg-num'});
  const inp = el('input', {type: 'number', value: String(value),
                           min: String(min), max: String(max)});
  inp.addEventListener('change', guard(async () => {
    const v = Number(inp.value);
    if (!Number.isFinite(v)) return;
    await editTrigger(file, ed.line, {value: Math.max(min, Math.min(max, v))});
  }));
  box.appendChild(inp);
  if (suffix) box.appendChild(el('span', {class: 'unit', text: suffix}));
  return box;
}

// The objective list is the interesting half: an id the campaign does not carry
// is silently skipped by the engine, so the server refuses one rather than let
// a condition quietly become unreachable.
function objectiveList(file, ed) {
  const box = el('span', {class: 'eg-objs'});
  const ids = ed.ids.slice();
  for (const p of ed.places) {
    box.appendChild(el('span', {class: 'chip'}, [
      el('span', {text: p.name}),
      el('button', {
        class: 'chip-x', title: 'remove', text: '\u00d7',
        onclick: guard(async () => {
          if (ids.length <= 1) {
            toast('an #IF_CONTROLLED needs at least one objective, or it can ' +
                  'never fire', 'err');
            return;
          }
          await editTrigger(file, ed.line,
                            {ids: ids.filter(i => i !== p.campId)});
        }),
      }),
    ]));
  }
  const add = el('input', {type: 'number', class: 'chip-add',
                           placeholder: '+ camp id'});
  add.addEventListener('change', guard(async () => {
    const v = Number(add.value);
    add.value = '';
    if (!Number.isFinite(v) || ids.indexOf(v) >= 0) return;
    await editTrigger(file, ed.line, {ids: ids.concat([v])});
  }));
  box.appendChild(add);
  return box;
}

// --- the text the campaign-select page shows ---------------------------------

function campaignTextPanel(file, tri) {
  const t = tri.text || {};
  const body = el('div', {class: 'panel-body'});

  if (!t.available) {
    body.appendChild(el('p', {class: 'note warn', text:
      'No editable selection text \u2014 ' + (t.reason || 'not found') + '.'}));
    return el('div', {class: 'panel'}, [
      el('header', {}, el('h3', {text: 'Campaign selection text'})), body]);
  }

  body.appendChild(el('p', {class: 'note', text:
    'The campaign-select page reads these two strings from ' + t.path +
    '. Nothing checks them against the script above, so they drift \u2014 this ' +
    'is the only place the two can be reconciled.'}));

  if (t.sharedWith && t.sharedWith.length) {
    body.appendChild(el('p', {class: 'note warn', text:
      'This file is shared with ' + t.sharedWith.join(', ') + ' \u2014 those ' +
      'theaters point artdir at the same art tree, so editing here changes ' +
      'their campaign text too.'}));
  }

  const save = (key, value) => api('/api/campaign/text',
    {theater: S.theater, file: file, [key]: value})
    .then(refreshPending)
    .then(() => toast('campaign ' + key + ' updated'));

  const nameIn = el('input', {type: 'text', class: 'wide'});
  nameIn.value = t.name;
  nameIn.addEventListener('change', guard(() => save('name', nameIn.value)));

  const blurbIn = el('textarea', {class: 'wide', rows: '3'});
  blurbIn.value = t.blurb;
  blurbIn.addEventListener('change', guard(() => save('blurb', blurbIn.value)));

  body.appendChild(el('label', {class: 'stack'}, [
    el('span', {class: 'lab', text: t.nameKey + ' \u2014 campaign name'}),
    nameIn,
  ]));
  body.appendChild(el('label', {class: 'stack'}, [
    el('span', {class: 'lab',
                text: t.blurbKey + ' \u2014 SitRep / victory text'}),
    blurbIn,
  ]));

  const tidy = x => String(x || '').replace(/\s+/g, ' ').trim().toLowerCase();
  const matches = tri.suggestedBlurb && tidy(t.blurb) === tidy(tri.suggestedBlurb);

  body.appendChild(el('div', {class: 'suggest'}, [
    el('div', {class: 'lab', text: matches
      ? 'This matches what the script implements.'
      : 'What the script above actually implements:'}),
    el('div', {class: 'suggest-text', text: tri.suggestedBlurb}),
    matches ? null : el('button', {
      class: 'btn', text: 'Use this text',
      onclick: guard(async () => {
        await save('blurb', tri.suggestedBlurb);
        await drawView();
      }),
    }),
  ]));

  return el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: 'Campaign selection text'}),
      el('span', {class: 'hint', text: t.file + '  \u00b7  slot ' + t.slot}),
    ]),
    body,
  ]);
}

async function victoryPanel(file) {
  const c = await campaignData(file);
  const wrap = el('div', {class: 'pad'});
  const changed = {};
  const F = k => fieldEditor(c, file, changed, k);

  // How the campaign actually ends: the .tri script, not the header.
  let tri = null;
  try { tri = await api('/api/triggers?' + qs({file: file})); } catch (e) {}

  if (tri && tri.available) {
    const body = el('div', {class: 'panel-body'});
    body.appendChild(el('p', {class: 'note', text:
      tri.fromScenario
        ? 'The engine reads the trigger script named by the Scenario field in '
          + 'this file\u2019s header, so this campaign ends by ' + tri.file
          + ' \u2014 the script of ' + tri.fromScenario + '. Each block below '
          + 'is a #END_GAME and the conditions that reach it.'
        : 'A campaign carries no victory field. What ends it is ' + tri.file +
          ', a script the engine evaluates every tick \u2014 each block below ' +
          'is a #END_GAME and the conditions that reach it. EndgameResult on ' +
          'this page only records which one fired.'}));

    if (tri.fromScenario) {
      body.appendChild(el('p', {class: 'note warn', text:
        tri.ownObjectives
          ? 'This file ships its own script, but the engine does not read it: '
            + 'CheckTriggers uses the Scenario field in the header, which names '
            + tri.fromScenario + '. Editing below rewrites ' + tri.file
            + ', the script that actually applies. To use this file\u2019s own '
            + 'script instead, change Scenario on the Campaign tab.'
          : 'A mid-campaign save records objective deltas against '
            + tri.fromScenario + ', not a scenario of its own, so it cannot '
            + 'carry its own ending. Editing below rewrites ' + tri.file
            + ' and changes every campaign and save started from '
            + tri.fromScenario + '.'}));
      if (!tri.ownObjectives) {
        body.appendChild(el('div', {class: 'stores-tools'}, [
          el('button', {class: 'btn btn-sm btn-primary',
            text: 'Give this save its own ending\u2026',
            onclick: () => openSaveEnding(file)}),
          el('span', {class: 'hint', text:
            'copies the scenario, points this save at the copy, then edit the ' +
            'script here without touching ' + tri.fromScenario}),
        ]));
      }
    }

    for (const e of tri.endgames) {
      body.appendChild(endgameCard(file, tri, e));
    }
    if (!tri.endgames.length) {
      body.appendChild(el('p', {class: 'note warn', text:
        'This script has no #END_GAME at all, so the campaign can never end ' +
        'on its own.'}));
    }

    wrap.appendChild(el('div', {class: 'panel'}, [
      el('header', {}, [
        el('h3', {text: 'How this campaign ends'}),
        el('span', {class: 'hint',
          text: tri.file + (tri.fromScenario ? ' (from ' + tri.fromScenario + ')'
                                             : '') +
                '  \u00b7  ' + tri.totalEvents + ' events  \u00b7  ' +
                tri.watched.length + ' objectives watched'}),
      ]),
      body,
    ]));

    if (tri.watched.length) {
      wrap.appendChild(el('div', {class: 'panel'}, [
        el('header', {}, [
          el('h3', {text: 'Objectives the script watches'}),
          el('span', {class: 'hint',
            text: 'every place named by an #IF_CONTROLLED'}),
        ]),
        el('table', {class: 'data'}, [
          el('thead', {}, el('tr', {}, ['Camp id', 'Name', 'Type', 'Owner',
                                        'Position'].map(h => el('th', {text: h})))),
          el('tbody', {}, tri.watched.map(w => el('tr', {
            onclick: guard(async () => {
              S.tab = 'map';
              drawTabs();
              await drawView();
              if (typeof mapFocus === 'function') mapFocus(w.x, w.y);
            }),
          }, [
            el('td', {class: 'idx', text: String(w.campId)}),
            el('td', {}, [
              el('span', {class: 'swatch',
                style: 'background:' + teamColour(w.owner)}),
              ' ' + (w.name || ('objective ' + w.campId)),
            ]),
            el('td', {text: w.type}),
            el('td', {text: (c.teams[w.owner] || {}).name || ('team ' + w.owner)}),
            el('td', {class: 'num', text: w.x + ', ' + w.y}),
          ]))),
        ]),
      ]));
    }

    wrap.appendChild(campaignTextPanel(file, tri));

    // Which scripted events have fired in this file. A scenario has none; a
    // save carries the flags (they say that an event fired, not when).
    if (tri.events && tri.events.length) {
      const nFired = tri.events.filter(e => e.fired).length;
      wrap.appendChild(el('div', {class: 'panel'}, [
        el('header', {}, [
          el('h3', {text: 'Events fired'}),
          el('span', {class: 'hint', text: tri.eventsFromSave
            ? nFired + ' of ' + tri.events.length + ' in this file'
            : 'no saved state'}),
        ]),
        el('div', {class: 'panel-body'}, [
          el('div', {class: 'ev-grid'}, tri.events.map(e => el('div', {
            class: 'ev-chip' + (e.fired ? ' fired' : ''),
            title: e.fired ? 'fired in this file' : 'not fired',
          }, [
            el('b', {text: '#' + e.id}),
            el('span', {text: ' ' + (e.title || '')}),
            el('em', {text: e.fired ? (e.day ? 'Day ' + e.day + ' ' + e.clock : 'FIRED') : ''}),
          ]))),
          (tri.eventHistory && tri.eventHistory.length) ? el('div', {class: 'ev-hist'}, tri.eventHistory.map(h => el('div', {}, [
            el('b', {text: 'Day ' + h.day + ' ' + h.clock}),
            el('span', {text: (h.kind === 'movie' ? '  news clip: ' : '  event: ') + (h.title || ('#' + h.id))}),
          ]))) : null,
          el('p', {class: 'note', text:
            'Open an Auto Save or any save to see what has happened in that ' +
            'campaign; a fresh scenario shows nothing fired. Saves made with the ' +
            'current game also record when each event fired.'}),
        ]),
      ]));
    }

    // Which branch ran. Two branches often fire the same event (China joins on low supply OR on
    // a lost air war), so when the save says which #IF chain led to an action, light only the
    // rows on that chain rather than every write of a fired event.
    const hasBranches = tri.outline.some(r => r.taken && r.taken.length);
    const onPath = [];
    const listing = el('div', {class: 'panel-body script'});
    for (const row of tri.outline) {
      const taken = row.taken || [];
      const inside = onPath.slice(0, row.depth).every(Boolean);
      if (row.kind === 'condition')
        onPath[row.depth] = taken.some(t => t.branch === 'if');
      else if (row.kind === 'else')
        onPath[row.depth] = taken.length > 0;
      onPath.length = row.depth + 1;
      const lit = hasBranches ? (row.fired && inside) : row.fired;
      const shown = row.kind === 'condition' ? taken.filter(t => t.branch === 'if') : taken;
      const failed = row.kind === 'condition' ? taken.filter(t => t.branch === 'else') : [];
      const badge = (t, ok) => el('span', {
        class: 'sl-taken' + (ok ? '' : ' no') + (t.source === 'recorded' ? '' : ' guess'),
        title: t.source === 'recorded'
          ? 'recorded by the game when the action ran'
          : t.source === 'reconstructed'
            ? 're-evaluated from the hourly team stats saved beside this file (.frc)'
            : 'consistent with the saved stats, but this save cannot show the rest of the chain',
        text: (ok ? '✓ ' : '✗ ') + 'Day ' + t.day + ' ' + t.clock +
              (t.measured ? ' — ' + t.measured : '') +
              (t.source === 'possible' ? ' (possibly)' : ''),
      });
      listing.appendChild(el('div', {
        class: 'sl sl-' + row.kind + (lit ? ' sl-fired' : ''),
        style: 'padding-left:' + (row.depth * 18) + 'px',
      }, [
        row.comment
          ? el('div', {class: 'sl-note', text: row.comment}) : null,
        el('span', {class: 'sl-verb', text: row.verb}),
        el('span', {class: 'sl-text', text: row.text}),
        row.note ? el('span', {class: 'sl-flag', text: row.note}) : null,
        ...shown.map(t => badge(t, true)),
        ...failed.map(t => badge(t, false)),
      ]));
    }
    wrap.appendChild(el('div', {class: 'panel'}, [
      el('header', {}, [
        el('h3', {text: 'Full trigger script'}),
        el('span', {class: 'hint', text: 'read-only'}),
      ]),
      tri.writeOnlyEvents && tri.writeOnlyEvents.length
        ? el('div', {class: 'panel-body'},
            el('p', {class: 'note warn', text:
              'Event' + (tri.writeOnlyEvents.length === 1 ? ' ' : 's ') +
              tri.writeOnlyEvents.join(', ') + ' ' +
              (tri.writeOnlyEvents.length === 1 ? 'is' : 'are') + ' set but ' +
              'never tested by an #IF_EVENT_PLAYED, so nothing acts on ' +
              (tri.writeOnlyEvents.length === 1 ? 'it' : 'them') +
              ' \u2014 the movie beside the write is all that happens.'}))
        : null,
      listing,
    ]));
  } else {
    wrap.appendChild(el('div', {class: 'panel'}, [
      el('header', {}, el('h3', {text: 'How this campaign ends'})),
      el('div', {class: 'panel-body'},
        el('p', {class: 'note warn', text:
          'No trigger script for this campaign' +
          (tri && tri.reason ? ' \u2014 ' + tri.reason : '') +
          '. Without one nothing sets EndgameResult, so the campaign runs ' +
          'until you abandon it.'})),
    ]));
  }

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: 'Tactical engagement'}),
      el('span', {class: 'hint', text: 'clock and scoring'}),
    ]),
    el('div', {class: 'panel-body'}, [
      el('p', {class: 'note', text:
        'These are the only explicit win conditions in the format. A full ' +
        'campaign has no single victory scalar: EndgameResult records who won ' +
        'once it is over, and the war itself is driven by the supply and ' +
        'production model, so the starting ratios and Tempo on the Campaign ' +
        'tab are the real levers.'}),
      el('div', {class: 'grid4'},
        ['TE_StartTime', 'TE_TimeLimit', 'TE_VictoryPoints', 'TE_flags'].map(F)),
      el('div', {class: 'grid4'},
        ['TE_type', 'TE_number_teams', 'TE_team', 'EndgameResult'].map(F)),
    ]),
  ]));

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, el('h3', {text: 'Bullseye'})),
    el('div', {class: 'panel-body'},
      el('div', {class: 'grid3'},
        ['BullseyeName', 'BullseyeX', 'BullseyeY'].map(F))),
  ]));

  const pts = c.fields.TE_team_pts || [];
  const ac = c.fields.TE_number_aircraft || [];
  const f16 = c.fields.TE_number_f16s || [];
  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [el('h3', {text: 'Per-team score'}),
                      el('span', {class: 'hint', text: 'read-only'})]),
    el('table', {class: 'data'}, [
      el('thead', {}, el('tr', {}, ['Team', 'Points', 'Aircraft', 'F-16s']
        .map(h => el('th', {text: h})))),
      el('tbody', {}, c.teams.map((t, i) => el('tr', {}, [
        el('td', {text: t.name || ('team ' + i)}),
        el('td', {class: 'num', text: String(pts[i] || 0)}),
        el('td', {class: 'num', text: String(ac[i] || 0)}),
        el('td', {class: 'num', text: String(f16[i] || 0)}),
      ]))),
    ]),
  ]));

  return wrap;
}

// --- teams ------------------------------------------------------------------

async function teamsPanel(file) {
  const c = await campaignData(file);
  const wrap = el('div', {class: 'pad'});
  const queued = [];
  const push = guard(async () => {
    if (!queued.length) return;
    await pushCampaign(file, null, queued.slice());
    queued.length = 0;
    await refreshPending();
  });
  const mark = (idx, key, value) => {
    let e = queued.find(x => x.index === idx);
    if (!e) { e = {index: idx}; queued.push(e); }
    e[key] = value;
  };

  c.teams.forEach((t, idx) => {
    const body = el('div', {class: 'panel-body'}, [
      el('div', {class: 'grid3'}, [
        el('label', {class: 'field'}, [el('span', {text: 'name'}),
          el('input', {type: 'text', value: t.name,
            oninput: e => { mark(idx, 'name', e.target.value);
                            e.target.classList.add('changed'); },
            onchange: push})]),
        el('label', {class: 'field'}, [el('span', {text: 'flag'}),
          el('input', {type: 'number', value: t.flag,
            oninput: e => { mark(idx, 'flag', Number(e.target.value));
                            e.target.classList.add('changed'); },
            onchange: push})]),
        el('label', {class: 'field'}, [el('span', {text: 'colour'}),
          el('input', {type: 'number', value: t.colour,
            oninput: e => { mark(idx, 'colour', Number(e.target.value));
                            e.target.classList.add('changed'); },
            onchange: push})]),
      ]),
      el('label', {class: 'field'}, [el('span', {text: 'motto'}),
        el('textarea', {
          oninput: e => { mark(idx, 'motto', e.target.value);
                          e.target.classList.add('changed'); },
          onchange: push}, t.motto)]),
    ]);
    wrap.appendChild(el('div', {class: 'panel'}, [
      el('header', {}, [
        el('h3', {text: 'Team ' + idx + (t.name ? '   —   ' + t.name : '')}),
        el('span', {class: 'swatch-lg',
          style: 'background:' + (typeof teamColour === 'function'
                                  ? teamColour(idx) : '#666')}),
      ]),
      body,
    ]));
  });
  return wrap;
}

// --- unit and objective lists -----------------------------------------------

// --- sortable, filterable tables --------------------------------------------

// A column is {key, label, value: row => cell, filter: 'text' | 'number' | false}.
// `state` is {sort, dir, filters} and lives on the panel, so it survives the
// redraws that follow an edit. `onChange` re-renders a client-side table, or
// re-fetches one that pages on the server (the Database tab).
function tableState(store, key) {
  if (!store[key]) store[key] = {sort: null, dir: 1, filters: {}};
  return store[key];
}

function tableHead(columns, state, onChange) {
  const head = el('tr');
  for (const c of columns) {
    const sortable = c.sort !== false;
    const active = sortable && state.sort === c.key;
    const th = el('th', {class: sortable ? 'sortable' : ''});

    // The sort target is the label, not the whole cell: the filter input lives
    // in the same cell and must not re-sort when it is clicked.
    th.appendChild(el('div', {
      class: 'col-label' + (active ? ' sorted' : ''),
      title: sortable ? 'Sort by ' + (c.label || 'this column') : null,
      onclick: sortable ? () => {
        if (state.sort === c.key) state.dir = -(state.dir || 1);
        else { state.sort = c.key; state.dir = 1; }
        onChange();
      } : null,
    }, [c.label, sortable ? el('span', {class: 'arrow',
      text: active ? (state.dir < 0 ? ' ▾' : ' ▴') : ''}) : null]));

    if (c.filter !== false) {
      th.appendChild(el('input', {
        class: 'col-filter',
        type: 'text',
        value: state.filters[c.key] || '',
        placeholder: c.filter === 'number' ? '> 40' : 'filter',
        spellcheck: 'false',
        oninput: e => { state.filters[c.key] = e.target.value; onChange(); },
      }));
    }
    head.appendChild(th);
  }
  return el('thead', {}, head);
}

// One column's filter: a substring, or a numeric comparison like "> 40".
function matchFilter(value, want) {
  const text = String(value === null || value === undefined ? '' : value)
    .toLowerCase();
  const m = /^(>=|<=|>|<|=)\s*(-?\d+(?:\.\d+)?)$/.exec(want);
  if (m) {
    const n = Number(value);
    if (Number.isFinite(n)) {
      const v = Number(m[2]);
      if (m[1] === '>') return n > v;
      if (m[1] === '<') return n < v;
      if (m[1] === '>=') return n >= v;
      if (m[1] === '<=') return n <= v;
      return n === v;
    }
  }
  return text.includes(want);
}

function tableRows(rows, columns, state) {
  let out = rows;
  for (const c of columns) {
    const want = (state.filters[c.key] || '').trim().toLowerCase();
    if (!want || c.filter === false) continue;
    out = out.filter(r => matchFilter(c.value(r), want));
  }
  const col = columns.find(c => c.key === state.sort);
  if (col) {
    out = out.slice().sort((a, b) => {
      const av = col.value(a), bv = col.value(b);
      let d;
      if (typeof av === 'number' && typeof bv === 'number') d = av - bv;
      else d = String(av === null || av === undefined ? '' : av)
        .localeCompare(String(bv === null || bv === undefined ? '' : bv),
                       undefined, {numeric: true, sensitivity: 'base'});
      return d * (state.dir < 0 ? -1 : 1);
    });
  }
  return out;
}

async function unitsPanel(file) {
  const d = await api('/api/map?' + qs({file: file}));
  M.lastTeams = d.teams;
  const state = unitsPanel.state = unitsPanel.state || {q: '', team: -1, kind: ''};
  const tstate = tableState(state, 'cols');

  const detail = el('div', {class: 'detail'},
    el('div', {class: 'pad', style: 'color:var(--ink-faint)'},
       'Select a unit to inspect it.'));

  // A bar and a number for the 0..100 condition columns, red to green, so a
  // tired or hungry unit stands out while scrolling. Fatigue reads the other
  // way round: high is bad.
  const meter = (v, invert) => {
    if (v === null || v === undefined) return null;
    const good = invert ? 100 - v : v;
    const hue = Math.round(Math.max(0, Math.min(100, good)) * 1.2);
    return el('span', {class: 'meter'}, [
      el('span', {class: 'meter-bar', style: 'width:' + Math.max(0,
        Math.min(100, v)) + '%;background:hsl(' + hue + ',55%,45%)'}),
      el('span', {class: 'meter-num', text: String(v)}),
    ]);
  };
  const orNone = v => (v === null || v === undefined ? null : v);

  const columns = [
    {key: 'n', label: '#', value: u => u.n, filter: 'number',
      cell: u => el('td', {class: 'idx', text: String(u.n)})},
    {key: 'name', label: 'Name', value: u => u.sqName || u.name || u.kind,
      cell: u => el('td', {}, [
        el('span', {class: 'swatch', style: 'background:' + teamColour(u.owner)}),
        ' ' + (u.sqName || u.name || u.kind),
        u.isPlayer ? el('span', {class: 'tag', text: 'your squadron'}) : null])},
    {key: 'type', label: 'Type', value: u => u.aircraft || u.kind},
    {key: 'team', label: 'Team',
      value: u => (d.teams[u.owner] || {}).name || ('team ' + u.owner)},
    {key: 'health', label: 'Health', value: u => orNone(u.health),
      filter: 'number', title: 'vehicles left, % of full strength',
      cell: u => el('td', {}, meter(u.health))},
    {key: 'supply', label: 'Supply', value: u => orNone(u.supply),
      filter: 'number', cell: u => el('td', {}, meter(u.supply))},
    {key: 'morale', label: 'Morale', value: u => orNone(u.morale),
      filter: 'number', cell: u => el('td', {}, meter(u.morale))},
    {key: 'fatigue', label: 'Fatigue', value: u => orNone(u.fatigue),
      filter: 'number', cell: u => el('td', {}, meter(u.fatigue, true))},
    {key: 'status', label: 'Status', value: u => (u.naval ? u.naval.state : '')},
    {key: 'elements', label: 'Elements', value: u => makeupText(u.makeup),
      cell: u => el('td', {class: 'makeup', text: makeupText(u.makeup)})},
    {key: 'patch', label: 'Patch', filter: false, sort: false,
      value: u => (u.kind === 'squadron' ? 'x' : ''),
      cell: u => el('td', {}, unitPatchThumb(u))},
    {key: 'home', label: 'Home', value: u => u.home || ''},
    {key: 'pos', label: 'Position', value: u => u.x + ', ' + u.y,
      cell: u => el('td', {class: 'num', text: u.x + ', ' + u.y})},
    {key: 'campId', label: 'Camp id', value: u => u.campId, filter: 'number',
      cell: u => el('td', {class: 'num', text: String(u.campId)})},
    {key: 'wp', label: 'WP', value: u => u.wp || 0, filter: 'number',
      cell: u => el('td', {class: 'num', text: u.wp ? String(u.wp) : ''})},
  ];

  // Which columns show. Remembered in this browser; a column with nothing to
  // show for the rows on screen (Patch and Home on battalions, the condition
  // columns on flights) steps aside by itself.
  const HIDE_KEY = 'ffcamp.units.hidden';
  let hidden;
  try { hidden = new Set(JSON.parse(localStorage.getItem(HIDE_KEY) ||
                                    '["pos","campId","wp"]')); }
  catch (e) { hidden = new Set(['pos', 'campId', 'wp']); }
  const saveHidden = () => {
    try { localStorage.setItem(HIDE_KEY, JSON.stringify([...hidden])); }
    catch (e) { /* private window: keep it for this page only */ }
  };
  const allColumns = columns;
  let shownColumns = allColumns;
  let headSig = '';

  const body = el('tbody');
  const stat = el('span', {class: 'stat'});

  const openDetail = guard(async (u, row) => {
    for (const tr of body.children) tr.classList.remove('on');
    if (row) {
      row.classList.add('on');
      // The panel may not be in the document yet when a row is opened from
      // another tab, and scrollIntoView on a detached node does nothing.
      setTimeout(() => row.scrollIntoView({block: 'nearest'}), 0);
    }
    const info = await api('/api/unit?' + qs({file: file, n: u.n}));
    detail.textContent = '';
    detail.appendChild(await unitDetail(info, u, file, () => {},
      guard(async () => { await drawView(); })));
  });

  const redraw = () => {
    body.textContent = '';
    const needle = state.q.trim().toLowerCase();
    const searched = d.units.filter(u =>
      (state.team < 0 || u.owner === state.team) &&
      (!state.kind || u.kind === state.kind) &&
      (!needle || unitHaystack(u).includes(needle)));
    const shown = tableRows(searched, allColumns, tstate);

    // Only the chosen columns, and of those only ones with something to show.
    const onScreen = shown.slice(0, 800);
    shownColumns = allColumns.filter(c => !hidden.has(c.key) &&
      (c.key === 'n' || c.key === 'name' || onScreen.some(u => {
        const v = c.value(u);
        return v !== null && v !== undefined && v !== '';
      })));
    // Rebuild the header only when the set of columns or the sort changes:
    // rebuilding it on every keystroke would take the focus out of the column
    // filter being typed in.
    const sig = shownColumns.map(c => c.key).join(',') + '|' + tstate.sort +
                (tstate.dir || 1);
    if (sig !== headSig) {
      headSig = sig;
      const fresh = tableHead(shownColumns, tstate, redraw);
      if (thead) thead.replaceWith(fresh);
      thead = fresh;
    }
    // Your squadron leads the list, whatever the sort, as on the Squadrons tab.
    shown.sort((a, b) => (b.isPlayer ? 1 : 0) - (a.isPlayer ? 1 : 0));
    stat.textContent = shown.length.toLocaleString() + ' of ' +
                       d.units.length.toLocaleString() +
                       (shown.length > 800 ? '   ·   first 800 shown' : '');
    for (const u of shown.slice(0, 800)) {
      const t = d.teams[u.owner];
      const tr = el('tr', {
        'data-n': String(u.n),
        class: u.isPlayer ? 'mine' : '',
        onclick: guard(async () => { await openDetail(u, tr); }),
      }, shownColumns.map(c => c.cell ? c.cell(u)
        : el('td', {text: String(c.value(u) === null || c.value(u) === undefined
                                 ? '' : c.value(u))})));
      body.appendChild(tr);
    }
  };

  const search = el('input', {type: 'text', value: state.q, spellcheck: 'false',
    placeholder: 'Search name, aircraft, base, camp id…',
    oninput: e => { state.q = e.target.value; redraw(); }});
  const teamSel = el('select', {onchange: e => {
    state.team = Number(e.target.value); redraw();
  }}, [el('option', {value: -1, selected: state.team < 0}, 'All teams')].concat(
    d.teams.filter(t => t.name && t.name !== 'XX').map(t =>
      el('option', {value: t.index, selected: t.index === state.team}, t.name))));
  const kindSel = el('select', {onchange: e => {
    state.kind = e.target.value; redraw();
  }}, [el('option', {value: ''}, 'All kinds')].concat(
    [...new Set(d.units.map(u => u.kind))].sort().map(k =>
      el('option', {value: k, selected: k === state.kind}, k))));

  let thead = null;
  redraw();

  // The column picker: a checkbox per column, kept across visits.
  const colMenu = el('details', {class: 'col-picker'}, [
    el('summary', {class: 'btn btn-sm', text: 'Columns'}),
    el('div', {class: 'col-picker-list'}, allColumns
      .filter(c => c.key !== 'n' && c.key !== 'name')
      .map(c => el('label', {class: 'chk'}, [
        el('input', {type: 'checkbox', checked: !hidden.has(c.key),
          onchange: e => {
            if (e.target.checked) hidden.delete(c.key);
            else hidden.add(c.key);
            saveHidden();
            redraw();
          }}),
        el('span', {text: c.label}),
      ]))),
  ]);

  // The squadron this file says you last flew (the header's PlayerSquadronID).
  // The button clears whatever filter would hide it, because a jump that
  // silently lands on nothing is worse than no button.
  const mine = d.units.find(u => u.isPlayer);
  const mineBtn = mine ? el('button', {class: 'btn btn-sm',
    title: (mine.sqName || mine.name) + (mine.home ? ' · ' + mine.home : ''),
    onclick: guard(async () => {
      if (!body.querySelector('[data-n="' + mine.n + '"]')) {
        state.q = ''; state.team = -1; state.kind = '';
        search.value = ''; teamSel.value = '-1'; kindSel.value = '';
        tstate.filters = {};
        headSig = '';
        redraw();
      }
      await openDetail(mine, body.querySelector('[data-n="' + mine.n + '"]'));
    })}, 'My squadron') : null;

  // A row opened from somewhere else (the Squadrons tab) is selected here.
  if (state.pick !== undefined && state.pick !== null) {
    const want = state.pick;
    state.pick = null;
    const u = d.units.find(x => x.n === want);
    if (u) await openDetail(u, body.querySelector('[data-n="' + want + '"]'));
  }

  return el('div', {class: 'browser'}, [
    el('div', {class: 'browser-list'}, [
      el('div', {class: 'toolbar'}, [
        search, teamSel, kindSel,
        el('span', {class: 'spacer'}),
        stat,
        colMenu,
        mineBtn,
        el('button', {class: 'btn btn-sm btn-primary',
          onclick: () => openPlacePicker(file)}, 'Place unit…'),
      ]),
      el('div', {class: 'rows'}, el('table', {class: 'data'}, [
        thead,
        body,
      ])),
    ]),
    detail,
  ]);
}

// Everything the Units search box should match, so a squadron can be found by
// its aircraft or its base as well as its name.
function unitHaystack(u) {
  return [u.sqName, u.name, u.aircraft, u.home, u.kind,
          (u.makeup || []).map(m => m.name).join(' '),
          String(u.campId)].join(' ').toLowerCase();
}

function makeupText(makeup) {
  if (!makeup || !makeup.length) return '';
  return makeup.map(m => m.count + ' × ' + m.name).join(', ');
}

function unitPatchThumb(u) {
  if (u.kind !== 'squadron' || u.patch === null || u.patch === undefined) {
    return null;
  }
  const img = el('img', {class: 'sq-thumb', alt: '',
    title: u.patchName || ('patch ' + u.patch),
    src: '/api/patch?theater=' + encodeURIComponent(S.theater)
         + '&i=' + u.patch});
  img.addEventListener('error', () => img.remove());
  return img;
}

async function objectivesPanel(file) {
  const d = await api('/api/map?' + qs({file: file}));
  M.lastTeams = d.teams;
  const state = objectivesPanel.state =
    objectivesPanel.state || {q: '', team: -1, cat: ''};
  const tstate = tableState(state, 'cols');

  const detail = el('div', {class: 'detail'},
    el('div', {class: 'pad', style: 'color:var(--ink-faint)'},
       'Select an objective to inspect it.'));

  const columns = [
    {key: 'n', label: '#', value: o => o.n, filter: 'number'},
    {key: 'name', label: 'Name', value: o => o.name || o.type},
    {key: 'type', label: 'Type', value: o => o.type},
    {key: 'cat', label: 'Category', value: o => d.categories[o.cat] || o.cat},
    {key: 'owner', label: 'Owner',
      value: o => (d.teams[o.owner] || {}).name || ('team ' + o.owner)},
    {key: 'pos', label: 'Position', value: o => o.x + ', ' + o.y},
    {key: 'tacan', label: 'TACAN', value: o => o.tacan || ''},
  ];

  const body = el('tbody');
  const stat = el('span', {class: 'stat'});

  const openDetail = guard(async (o, row) => {
    for (const tr of body.children) tr.classList.remove('on');
    if (row) { row.classList.add('on'); row.scrollIntoView({block: 'nearest'}); }
    const info = await api('/api/objective?' + qs({file: file, n: o.n}));
    detail.textContent = '';
    detail.appendChild(objectiveDetail(info, o, file, () => {},
      guard(async () => { await openDetail(o, row); })));
  });

  const redraw = () => {
    body.textContent = '';
    const needle = state.q.trim().toLowerCase();
    const searched = d.objectives.filter(o =>
      (state.team < 0 || o.owner === state.team) &&
      (!state.cat || o.cat === state.cat) &&
      (!needle || (o.name || '').toLowerCase().includes(needle) ||
       (o.type || '').toLowerCase().includes(needle)));
    const shown = tableRows(searched, columns, tstate);
    stat.textContent = shown.length.toLocaleString() + ' of ' +
                       d.objectives.length.toLocaleString() +
                       (shown.length > 800 ? '   ·   first 800 shown' : '');
    for (const o of shown.slice(0, 800)) {
      const t = d.teams[o.owner];
      const tr = el('tr', {
        'data-n': String(o.n),
        onclick: guard(async () => { await openDetail(o, tr); }),
      }, [
        el('td', {class: 'idx', text: String(o.n)}),
        el('td', {}, [
          el('span', {class: 'swatch',
            style: 'background:' + teamColour(o.owner)}),
          ' ' + (o.name || o.type),
        ]),
        el('td', {text: o.type}),
        el('td', {text: d.categories[o.cat] || o.cat}),
        el('td', {text: (t && t.name) || ('team ' + o.owner)}),
        el('td', {class: 'num', text: o.x + ', ' + o.y}),
        el('td', {class: 'num', text: o.tacan || ''}),
      ]);
      body.appendChild(tr);
    }
  };

  const search = el('input', {type: 'text', value: state.q, spellcheck: 'false',
    placeholder: 'Search name or type…',
    oninput: e => { state.q = e.target.value; redraw(); }});
  const teamSel = el('select', {onchange: e => {
    state.team = Number(e.target.value); redraw();
  }}, [el('option', {value: -1, selected: state.team < 0}, 'All teams')].concat(
    d.teams.filter(t => t.name && t.name !== 'XX').map(t =>
      el('option', {value: t.index, selected: t.index === state.team}, t.name))));
  const catSel = el('select', {onchange: e => {
    state.cat = e.target.value; redraw();
  }}, [el('option', {value: ''}, 'All categories')].concat(
    [...new Set(d.objectives.map(o => o.cat))].sort().map(c =>
      el('option', {value: c, selected: c === state.cat},
         d.categories[c] || c))));

  redraw();

  return el('div', {class: 'browser'}, [
    el('div', {class: 'browser-list'}, [
      el('div', {class: 'toolbar'}, [
        search, teamSel, catSel,
        el('span', {class: 'spacer'}),
        d.objectivesFrom !== file
          ? el('span', {class: 'stat warn', text: 'from ' + d.objectivesFrom})
          : null,
        stat,
      ]),
      el('div', {class: 'rows'}, el('table', {class: 'data'}, [
        tableHead(columns, tstate, redraw),
        body,
      ])),
    ]),
    detail,
  ]);
}

// --- squadrons --------------------------------------------------------------

async function squadronsPanel(file) {
  const c = await campaignData(file);
  const wrap = el('div', {class: 'pad'});
  if (!c.squadrons.length) {
    wrap.appendChild(el('div', {class: 'panel'}, [
      el('header', {}, el('h3', {text: 'Squadrons'})),
      el('div', {class: 'panel-body'},
        el('p', {class: 'note', text:
          'This campaign lists no selectable squadrons.'})),
    ]));
    return wrap;
  }

  const state = squadronsPanel.state = squadronsPanel.state || {};
  const tstate = tableState(state, 'cols');
  const rows = c.squadrons.map((s, i) => Object.assign({_i: i}, s));

  const columns = [
    {key: 'i', label: '#', value: s => s._i, filter: 'number'},
    {key: 'patch', label: 'Patch', filter: false, sort: false},
    {key: 'name', label: 'Name', value: s => s.name || ''},
    {key: 'aircraft', label: 'Aircraft', value: s => s.aircraft || ''},
    {key: 'airbase', label: 'Airbase', value: s => s.airbaseName || ''},
    {key: 'strength', label: 'Strength', value: s => s.currentStrength,
      filter: 'number'},
    {key: 'specialty', label: 'Specialty', value: s => s.specialtyName || ''},
    {key: 'country', label: 'Country', value: s => s.countryName || ''},
    {key: 'pos', label: 'Position',
      value: s => s.x.toFixed(0) + ', ' + s.y.toFixed(0)},
  ];

  // The header record and the unit share only a VU_ID, so that is the join:
  // find the unit with the same id and open it on the Units tab.
  const openUnit = async (s) => {
    const d = await api('/api/map?' + qs({file: file}));
    const hit = (d.units || []).find(u =>
      u.kind === 'squadron' && u.id && s.id &&
      u.id[0] === s.id[0] && u.id[1] === s.id[1]);
    if (!hit) {
      toast('No squadron unit carries this record\u2019s id', 'err');
      return;
    }
    unitsPanel.state = unitsPanel.state || {q: '', team: -1, kind: ''};
    unitsPanel.state.kind = 'squadron';
    unitsPanel.state.q = '';
    unitsPanel.state.pick = hit.n;
    S.tab = 'units';
    drawTabs();
    await drawView();
  };

  // What this file says you last flew. A fresh scenario records nothing
  // meaningful, and saying so beats pinning a squadron nobody chose.
  const mine = rows.find(s => s.isPlayer);
  let banner;
  if (mine) {
    const thumb = el('img', {class: 'sq-thumb', alt: '',
      src: '/api/patch?theater=' + encodeURIComponent(S.theater)
           + '&i=' + mine.squadronPatch});
    thumb.addEventListener('error', () => thumb.remove());
    banner = el('div', {class: 'mine-banner'}, [
      thumb,
      el('div', {}, [
        el('div', {class: 'lab', text: 'Your squadron'}),
        el('div', {class: 'who', text: [mine.name, mine.aircraft,
          mine.airbaseName].filter(Boolean).join('  \u00b7  ')}),
      ]),
      el('span', {class: 'spacer'}),
      el('button', {class: 'btn btn-sm btn-primary', text: 'Open it',
        onclick: guard(async () => { await openUnit(mine); })}),
    ]);
  } else {
    banner = el('p', {class: 'note', text: c.playerSquadron
      ? 'This file records no squadron you have flown yet \u2014 it is a '
        + 'fresh scenario, or the id it holds matches no squadron here.'
      : 'This file records no player squadron.'});
  }

  const body = el('tbody');
  const stat = el('span', {class: 'stat'});

  const redraw = () => {
    body.textContent = '';
    const shown = tableRows(rows, columns, tstate);
    // Your squadron (the header's PlayerSquadronID) always leads the list,
    // whatever the sort, so it is the first thing you see.
    shown.sort((a, b) => (b.isPlayer ? 1 : 0) - (a.isPlayer ? 1 : 0));
    stat.textContent = shown.length.toLocaleString() + ' of ' +
                       rows.length.toLocaleString() + ' shown';
    for (const s of shown) {
      const patch = el('img', {class: 'sq-thumb', alt: '',
        src: '/api/patch?theater=' + encodeURIComponent(S.theater)
             + '&i=' + s.squadronPatch});
      patch.addEventListener('error', () => patch.remove());

      // The header record and the unit share only a VU_ID, so that is the
      // join: find the unit with the same id and open it straight away.
      body.appendChild(el('tr', {class: 'clickable' + (s.isPlayer ? ' mine' : ''), onclick: guard(async () => { await openUnit(s); })}, [
        el('td', {class: 'idx', text: String(s._i)}),
        el('td', {}, patch),
        el('td', {}, [s.name || ('squadron ' + s._i),
          s.isPlayer ? el('span', {class: 'tag', text: 'your squadron'}) : null]),
        el('td', {text: s.aircraft || ''}),
        el('td', {text: s.airbaseName || ''}),
        el('td', {class: 'num', text: String(s.currentStrength)}),
        el('td', {text: s.specialtyName || String(s.specialty)}),
        el('td', {text: s.countryName || String(s.country)}),
        el('td', {class: 'num',
          text: s.x.toFixed(0) + ', ' + s.y.toFixed(0)}),
      ]));
    }
  };
  redraw();
  // This tab is the campaign HEADER's list of squadrons the player may fly --
  // a different thing from the squadron entities in the unit stream, which is
  // where stores, pilots and the airbase assignment live. Asking "where do I
  // change my squadron's weapons" and landing here is the obvious mistake, so
  // say where to go.
  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: 'Selectable squadrons'}),
      el('span', {class: 'hint',
        text: c.squadronCount + ' flyable in this campaign'}),
    ]),
    el('div', {class: 'panel-body'}, [
      banner,
      el('p', {class: 'note', text:
        'This is the roster the campaign-select screen offers the player: a '
        + 'record per flyable squadron, kept in the campaign header and '
        + 'separate from the squadron units themselves. It is what decides '
        + 'which squadrons you can fly, and it carries the patch and the '
        + 'airbase label shown on that screen.'}),
      el('p', {class: 'note', text:
        'Weapons, stores, pilots and kills belong to the squadron units. Click '
        + 'a row to open the matching unit, or take the whole list at once.'}),
      el('div', {class: 'toolbar'}, [
        el('button', {class: 'btn', text: 'Open all squadron units',
          onclick: guard(async () => {
            unitsPanel.state = unitsPanel.state || {q: '', team: -1, kind: ''};
            unitsPanel.state.kind = 'squadron';
            unitsPanel.state.q = '';
            S.tab = 'units';
            drawTabs();
            await drawView();
          })}),
        el('span', {class: 'spacer'}),
        stat,
      ]),
    ]),
    el('table', {class: 'data'}, [
      tableHead(columns, tstate, redraw),
      body,
    ]),
  ]));
  return wrap;
}

// --- file contents ----------------------------------------------------------

const MEMBER_WHAT = {
  cmp: 'Campaign header — the Campaign, Teams and Victory tabs',
  obj: 'Objective list — the Objectives tab',
  obd: 'Objective deltas against the base scenario',
  uni: 'Unit list — the Map and Units tabs',
  tea: 'Per-team data',
  evt: 'Event queue',
  plt: 'Pilot records',
  pst: 'Post-mission state',
  wth: 'Weather',
  pol: 'Political / PAK data',
  ver: 'Campaign data version',
  te: 'Tactical engagement header',
};

async function membersPanel(file) {
  const c = await campaignData(file);
  const wrap = el('div', {class: 'pad'});

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, [
      el('h3', {text: file}),
      el('span', {class: 'hint', text: 'data version ' + c.version}),
    ]),
    el('div', {class: 'panel-body'},
      el('p', {class: 'note', text:
        'A .cam is a container of named members. The editor reads all of ' +
        'them and writes back only the ones you changed — everything ' +
        'else is copied through byte for byte.'})),
    el('table', {class: 'data'}, [
      el('thead', {}, el('tr', {}, ['Member', 'Bytes', 'What it is']
        .map(h => el('th', {text: h})))),
      el('tbody', {}, c.members.map(m => el('tr', {}, [
        el('td', {class: 'mono', text: m.name}),
        el('td', {class: 'num', text: m.bytes.toLocaleString()}),
        el('td', {text: MEMBER_WHAT[m.name.split('.').pop().toLowerCase()] || ''}),
      ]))),
    ]),
  ]));

  wrap.appendChild(el('div', {class: 'panel'}, [
    el('header', {}, el('h3', {text: 'This file'})),
    el('div', {class: 'panel-body'}, [
      el('button', {class: 'btn', onclick: () => openNewCampaign(file)},
         'Duplicate as a new campaign…'),
      el('button', {class: 'btn btn-danger', style: 'margin-left:8px',
        onclick: guard(async () => {
          if (!window.confirm('Delete ' + file + '? A backup is kept.')) return;
          const res = await api('/api/campaign/delete',
                                {theater: S.theater, file: file});
          toast('Deleted ' + res.removed + ' (backup kept)', 'ok');
          S.file = null;
          S.tab = 'theater';
          S.info = await api('/api/theater?theater=' +
                             encodeURIComponent(S.theater));
          await loadState();
          drawTabs();
          await drawView();
        })}, 'Delete this campaign'),
    ]),
  ]));

  return wrap;
}

// --- database table browser -------------------------------------------------

let searchTimer = null;

async function tablePanel(name, query) {
  const q = query === undefined ? (tablePanel.q || '') : query;
  tablePanel.q = q;
  tablePanel.states = tablePanel.states || {};
  const tstate = tableState(tablePanel.states, name);

  const detail = el('div', {class: 'detail'},
    el('div', {class: 'pad', style: 'color:var(--ink-faint)'},
       'Select a row to edit it.'));

  const body = el('tbody');
  const table = el('table', {class: 'data'});
  const stat = el('span', {class: 'stat'});
  let columns = null;
  let editTimer = null;

  // Typing in a column filter should not fire a request per keystroke.
  const onChange = () => {
    clearTimeout(editTimer);
    editTimer = setTimeout(() => reload(), 200);
  };

  // Sorting and filtering happen on the server, because the table pages: a
  // client-side sort would only reorder the 400 rows that happen to be here.
  const reload = guard(async () => {
    const params = new URLSearchParams({
      theater: S.theater, table: name, q: q, limit: '400',
      sort: tstate.sort || '', dir: String(tstate.dir || 1),
      filters: JSON.stringify(tstate.filters || {}),
    });
    const data = await api('/api/table?' + params.toString());
    columns = dbTableColumns(data, name);

    table.textContent = '';
    table.appendChild(tableHead(columns, tstate, onChange));
    body.textContent = '';
    for (const r of data.rows) {
      body.appendChild(el('tr', {
        class: r._changed ? 'edited' : '',
        onclick: guard(async () => {
          S.rowIndex = r._i;
          for (const tr of body.children) tr.classList.remove('on');
          const idx = data.rows.indexOf(r);
          if (body.children[idx]) body.children[idx].classList.add('on');
          const panel = await rowDetail(name, r._i);
          detail.textContent = '';
          detail.appendChild(panel);
        }),
      }, [
        el('td', {class: 'idx', text: String(r._i)}),
        el('td', {text: r._name || ''}),
        ...data.columns.map(c => el('td', {
          class: typeof r[c] === 'number' ? 'num' : '',
          text: fmt(r[c]),
        })),
        name === 'class' ? el('td', {text: r._dtype}) : null,
      ]));
    }
    table.appendChild(body);
    stat.textContent =
      data.matched.toLocaleString() + ' of ' + data.total.toLocaleString() +
      ' rows' + (data.changed ? '   ·   ' + data.changed + ' edited' : '') +
      '   ·   ' + data.file;
    return data;
  });

  const search = el('input', {
    type: 'text', placeholder: 'Search name or value…', value: q,
    spellcheck: 'false',
    oninput: e => {
      tablePanel.q = e.target.value;
      clearTimeout(searchTimer);
      searchTimer = setTimeout(() => reload(), 220);
    },
  });

  const list = el('div', {class: 'browser-list'}, [
    el('div', {class: 'toolbar'}, [
      search,
      el('span', {class: 'spacer'}),
      stat,
      el('button', {class: 'btn btn-sm',
        title: 'Append a copy of the selected row',
        onclick: guard(async () => {
          const res = await api('/api/row/add', {
            theater: S.theater, table: name,
            copyFrom: S.rowIndex === null ? undefined : S.rowIndex,
          });
          toast('Added row ' + res.index +
                (S.rowIndex === null ? '' : ' as a copy of ' + S.rowIndex), 'ok');
          S.rowIndex = res.index;
          await afterEdit();
        })}, 'Add row'),
    ]),
    el('div', {class: 'rows'}, table),
  ]);

  const wrap = el('div', {class: 'browser'}, [list, detail]);
  const data = await reload();
  if (S.rowIndex !== null && data.rows.some(r => r._i === S.rowIndex)) {
    const idx = data.rows.findIndex(r => r._i === S.rowIndex);
    if (body.children[idx]) body.children[idx].classList.add('on');
    detail.textContent = '';
    detail.appendChild(await rowDetail(name, S.rowIndex));
  }
  return wrap;
}

// The Database tab's columns: the row index and name the server computes,
// then whichever summary columns that table shows.
function dbTableColumns(data, name) {
  const cols = [
    {key: '_i', label: '#', value: r => r._i, filter: 'number'},
    {key: '_name', label: 'Name', value: r => r._name || ''},
  ];
  for (const c of data.columns) {
    cols.push({key: c, label: c, value: r => r[c]});
  }
  if (name === 'class') {
    cols.push({key: '_dtype', label: 'data', value: r => r._dtype || ''});
  }
  return cols;
}

function fmt(v) {
  if (v === undefined || v === null) return '';
  if (Array.isArray(v)) return v.join(', ');
  if (typeof v === 'number' && !Number.isInteger(v)) return v.toFixed(3);
  return String(v);
}

// Fields grouped so the detail pane reads like a datasheet, not a dump.
const GROUPS = {
  unit: [
    ['Identity', ['Index', 'Name', 'Flags', 'IconIndex', 'SpecialIndex',
                  'Role', 'PtDataIndex']],
    ['Movement', ['MovementType', 'MovementSpeed', 'MaxRange', 'Fuel', 'Rate']],
    ['Order of battle', ['NumElements', 'VehicleType']],
    ['Combat', ['HitChance', 'Strength', 'Range', 'Detection', 'DamageMod',
                'RadarVehicle']],
    ['Mission scores', ['Scores']],
  ],
  vehicle: [
    ['Identity', ['Index', 'Name', 'NCTR', 'Flags', 'HitPoints',
                  'NumberOfPilots', 'CallsignIndex', 'CallsignSlots',
                  'EngineSound']],
    ['Performance', ['MaxSpeed', 'MaxWt', 'EmptyWt', 'FuelWt', 'FuelEcon',
                     'HighAlt', 'LowAlt', 'CruiseAlt', 'RCSfactor']],
    ['Hardpoints', ['Weapon', 'Weapons', 'RackFlags', 'VisibleFlags']],
    ['Combat', ['HitChance', 'Strength', 'Range', 'Detection', 'DamageMod',
                'RadarType']],
  ],
  weapon: [
    ['Identity', ['Index', 'Name', 'Flags', 'GuidanceFlags', 'Collective']],
    ['Effect', ['Strength', 'DamageType', 'Range', 'BlastRadius', 'MaxAlt',
                'FireRate', 'Rariety']],
    ['Physical', ['Weight', 'DragIndex', 'SimweapIndex', 'SimDataIdx',
                  'RadarType']],
    ['Hit chance', ['HitChance']],
  ],
  objective: [
    ['Identity', ['Index', 'Name', 'IconIndex', 'PtDataIndex']],
    ['Behaviour', ['DataRate', 'DeagDistance', 'Features', 'RadarFeature',
                   'FirstFeature']],
    ['Combat', ['Detection', 'DamageMod']],
  ],
  feature: [
    ['Identity', ['Index', 'Name', 'Flags', 'Priority']],
    ['Damage', ['HitPoints', 'RepairTime', 'DamageMod']],
    ['Geometry', ['Height', 'Angle']],
    ['Sensors', ['RadarType', 'Detection']],
  ],
  weaponlist: [['List', ['Name', 'WeaponID', 'Quantity']]],
  class: [
    ['Entry', ['id_', 'dataType', 'dataPtr', 'vehicleDataIndex', 'classInfo_']],
    ['Models', ['visType']],
    ['Vu', ['collisionType_', 'collisionRadius_', 'hitpoints_', 'bubbleRange_',
            'updateRate_', 'updateTolerance_', 'createPriority_',
            'managementDomain_', 'damageSeed_']],
    ['Flags', ['transferable_', 'private_', 'tangible_', 'collidable_',
               'global_', 'persistent_', 'majorRevisionNumber_',
               'minorRevisionNumber_', 'fineUpdateForceRange_',
               'fineUpdateMultiplier_']],
  ],
};

// A fixed char array has count > 1 but edits as one text box, not a grid.
const isVector = c => c && c.count > 1 && c.kind !== 'str';

// --- aircraft hardpoints -----------------------------------------------------

// `Weapon[hp]` is a ROW in FALCON4.WCD or, when `Weapons[hp]` is 255, in
// FALCON4.WLD -- the marker LoadoutWeapons checks. The two arrays are one
// edit: picking a weapon list has to write the 255 alongside the row, or the
// engine looks the same number up in the other table.
let WEAPON_LOOKUP = null;

async function loadWeaponLookup() {
  if (WEAPON_LOOKUP && WEAPON_LOOKUP.theater === S.theater) return WEAPON_LOOKUP;
  const [w, l] = await Promise.all([
    api('/api/lookup?theater=' + encodeURIComponent(S.theater) + '&kind=weapon'),
    api('/api/lookup?theater=' + encodeURIComponent(S.theater)
        + '&kind=weaponlist'),
  ]);
  const byName = new Map();
  for (const it of l.items) {
    // Row 0 of FALCON4.WLD has no name; offering it would just be a blank
    // entry in the picker.
    if (!it.name.trim()) continue;
    byName.set(('list: ' + it.name).trim().toLowerCase(),
               {row: it.row, list: true, name: it.name});
  }
  for (const it of w.items) {
    if (!it.name.trim()) continue;
    byName.set(it.name.trim().toLowerCase(),
               {row: it.row, list: false, name: it.name});
  }
  WEAPON_LOOKUP = {
    theater: S.theater, byName,
    weapons: w.items.filter(it => it.name.trim()),
    lists: l.items.filter(it => it.name.trim()),
  };
  return WEAPON_LOOKUP;
}

function weaponDatalist() {
  let dl = document.getElementById('weapon-options');
  // Rebuild when the theater changes: the weapon tables are per-theater.
  if (dl && dl.dataset.theater === S.theater) return dl;
  if (dl) dl.remove();
  dl = el('datalist', {id: 'weapon-options'});
  dl.dataset.theater = S.theater;
  for (const it of WEAPON_LOOKUP.weapons) {
    dl.appendChild(el('option', {value: it.name}));
  }
  for (const it of WEAPON_LOOKUP.lists) {
    dl.appendChild(el('option', {value: 'list: ' + it.name}));
  }
  document.body.appendChild(dl);
  return dl;
}

// One row per hardpoint: what it carries, and how many when fully supplied.
// `onCommit(weapon, shots)` receives both arrays and saves them together.
async function hardpointEditor(opts) {
  await loadWeaponLookup();
  weaponDatalist();

  const weapon = opts.weapon.slice();
  const shots = opts.shots.slice();
  const resolved = (opts.resolved || []).slice();

  const box = el('div', {class: 'group'});
  if (opts.title !== '') {
    box.appendChild(el('h4', {text: opts.title || 'Hardpoints'}));
  }
  box.appendChild(el('p', {class: 'note', text:
    'A slot whose count is 255 is a weapon list (FALCON4.WLD); otherwise it is '
    + 'a weapon row (FALCON4.WCD). Picking a list sets that marker for you.'}));

  for (let i = 0; i < weapon.length; i++) {
    let isList = shots[i] === 255;

    const name = el('input', {type: 'text', class: 'hp-name',
      spellcheck: 'false', list: 'weapon-options',
      value: resolved[i] || '', placeholder: '(empty)'});
    const count = el('input', {type: 'number', min: '0', max: '254',
      value: String(isList ? 255 : shots[i]), disabled: isList});
    const note = el('div', {class: 'name',
      text: isList ? 'weapon list' : (weapon[i] ? 'row ' + weapon[i] : '')});

    const commit = guard(async () => {
      await opts.onCommit(weapon.slice(), shots.slice());
    });

    name.addEventListener('change', guard(async () => {
      const text = name.value.trim();
      if (!text) {
        weapon[i] = 0;
        shots[i] = 0;
        count.value = '0';
        count.disabled = false;
        note.textContent = '';
        resolved[i] = '';
        await commit();
        return;
      }
      const hit = WEAPON_LOOKUP.byName.get(text.toLowerCase());
      if (!hit) {
        name.value = resolved[i] || '';
        toast('No weapon or weapon list is called ' + text, 'err');
        return;
      }
      weapon[i] = hit.row;
      isList = hit.list;
      if (isList) {
        shots[i] = 255;
        count.value = '255';
        count.disabled = true;
      } else if (shots[i] === 255) {
        shots[i] = 1;
        count.value = '1';
        count.disabled = false;
      }
      resolved[i] = isList ? 'list: ' + hit.name : hit.name;
      note.textContent = isList ? 'weapon list' : 'row ' + hit.row;
      await commit();
    }));

    count.addEventListener('change', guard(async () => {
      const v = Math.max(0, Math.min(254, Number(count.value) || 0));
      shots[i] = v;
      count.value = String(v);
      await commit();
    }));

    box.appendChild(el('div', {class: 'hpoint'}, [
      el('div', {class: 'hp', text: String(i)}),
      name, count, note,
    ]));
  }

  return box;
}

async function rowDetail(table, index) {
  const r = await api('/api/row?theater=' + encodeURIComponent(S.theater) +
                      '&table=' + encodeURIComponent(table) + '&index=' + index);
  const cols = {};
  for (const c of r.columns) cols[c.name] = c;

  const pendingVals = {};
  const push = guard(async () => {
    if (!Object.keys(pendingVals).length) return;
    await api('/api/edit', {theater: S.theater, table: table, index: index,
                            values: pendingVals});
    for (const k in pendingVals) delete pendingVals[k];
    await afterEdit(true);
  });

  const isChanged = nm =>
    r.original && JSON.stringify(r.original[nm]) !== JSON.stringify(r.values[nm]);

  const scalarInput = (nm) => {
    const c = cols[nm];
    const input = el('input', {
      type: c.type === 'text' ? 'text' : 'number',
      step: c.type === 'float' ? 'any' : '1',
      value: String(r.values[nm]),
      spellcheck: 'false',
      class: isChanged(nm) ? 'changed' : '',
      oninput: e => {
        pendingVals[nm] = c.type === 'text' ? e.target.value
                                            : Number(e.target.value);
        e.target.classList.add('changed');
      },
      onchange: push,
    });
    return el('label', {class: 'field'}, [
      el('span', {text: nm}), input,
      c.doc ? el('div', {class: 'field-doc', text: c.doc}) : null,
    ]);
  };

  const arrayInput = (nm) => {
    const c = cols[nm];
    const labels = r.labels[nm] || null;
    const vals = r.values[nm].slice();
    const resolved = r.resolved[nm] || null;

    if (resolved) {
      const rows = vals.map((v, i) => el('div', {class: 'hpoint'}, [
        el('div', {class: 'hp', text: String(i)}),
        el('input', {type: 'number', value: String(v),
          oninput: e => {
            vals[i] = Number(e.target.value);
            pendingVals[nm] = vals.slice();
            e.target.classList.add('changed');
          },
          onchange: push}),
        el('div', {}),
        el('div', {class: 'name', text: resolved[i] || ''}),
      ]));
      return el('div', {class: 'group'}, [
        el('h4', {text: nm + (c.doc ? ' — ' + c.doc : '')}),
        ...rows,
      ]);
    }

    const cells = vals.map((v, i) => el('label', {}, [
      el('span', {text: labels ? labels[i] : String(i)}),
      el('input', {type: 'number', step: c.type === 'float' ? 'any' : '1',
        value: String(v),
        oninput: e => {
          vals[i] = Number(e.target.value);
          pendingVals[nm] = vals.slice();
          e.target.classList.add('changed');
        },
        onchange: push}),
    ]));
    return el('div', {class: 'group'}, [
      el('h4', {text: nm + (c.doc ? ' — ' + c.doc : '')}),
      el('div', {class: 'arr'}, cells),
    ]);
  };

  const wrap = el('div');
  wrap.appendChild(el('div', {class: 'detail-head'}, [
    el('h3', {text: r.title || (table + ' ' + index)}),
    el('div', {class: 'sub mono', text: table + '[' + index + ']' +
      (r.resolved.dataTable ? '  →  ' + r.resolved.dataTable : '')}),
    el('div', {class: 'sub', text: 'Shared by every campaign in this theater'}),
    el('div', {class: 'row-actions'}, [
      el('button', {class: 'btn btn-sm', onclick: guard(async () => {
        await api('/api/row/revert',
                  {theater: S.theater, table: table, index: index});
        toast('Reverted row ' + index, 'ok');
        await afterEdit(true);
      })}, 'Revert row'),
      el('button', {class: 'btn btn-sm', onclick: guard(async () => {
        const res = await api('/api/row/add',
                              {theater: S.theater, table: table, copyFrom: index});
        S.rowIndex = res.index;
        toast('Duplicated to row ' + res.index, 'ok');
        await afterEdit();
      })}, 'Duplicate'),
    ]),
  ]));

  const groups = GROUPS[table];
  const done = new Set();

  // The vehicle table's Weapon/Weapons pair is one control, not two arrays:
  // the 255 in Weapons decides which table Weapon is a row in.
  if (table === 'vehicle' && cols.Weapon && cols.Weapons) {
    wrap.appendChild(await hardpointEditor({
      weapon: r.values.Weapon,
      shots: r.values.Weapons,
      resolved: r.resolved.Weapon || [],
      title: 'Hardpoints',
      onCommit: async (weapon, shots) => {
        await api('/api/edit', {theater: S.theater, table: table, index: index,
                                values: {Weapon: weapon, Weapons: shots}});
        await afterEdit(true);
      },
    }));
    done.add('Weapon');
    done.add('Weapons');
  }

  const emitField = (nm, into) => {
    if (!cols[nm] || done.has(nm)) return;
    done.add(nm);
    if (isVector(cols[nm])) into.appendChild(arrayInput(nm));
    else into.appendChild(scalarInput(nm));
  };

  if (groups) {
    for (const [title, names] of groups) {
      const scalars = names.filter(n => cols[n] && !isVector(cols[n]));
      const arrays = names.filter(n => cols[n] && isVector(cols[n]));
      if (scalars.length) {
        wrap.appendChild(el('div', {class: 'group'}, [
          el('h4', {text: title}),
          el('div', {class: 'grid2'},
            scalars.map(n => { done.add(n); return scalarInput(n); })),
        ]));
      } else if (arrays.length) {
        wrap.appendChild(el('div', {class: 'group'}, el('h4', {text: title})));
      }
      for (const n of arrays) emitField(n, wrap);
    }
  }

  const rest = r.columns.map(c => c.name).filter(n => !done.has(n));
  if (rest.length) {
    const scalars = rest.filter(n => !isVector(cols[n]));
    if (scalars.length) {
      wrap.appendChild(el('div', {class: 'group'}, [
        el('h4', {text: groups ? 'Other' : 'Fields'}),
        el('div', {class: 'grid2'}, scalars.map(scalarInput)),
      ]));
      scalars.forEach(n => done.add(n));
    }
    for (const n of rest) emitField(n, wrap);
  }

  return wrap;
}

// --- mutation plumbing ------------------------------------------------------

// Map edits repaint the canvas themselves, so only the chrome needs updating.
async function refreshPending() {
  const st = await api('/api/state');
  S.pending = st.pending;
  drawPending();
  if (S.theater) {
    S.info = await api('/api/theater?theater=' + encodeURIComponent(S.theater));
    drawRail();
    drawTabs();
  }
}

async function afterEdit() {
  const keep = S.rowIndex;
  S.info = await api('/api/theater?theater=' + encodeURIComponent(S.theater));
  await loadState();
  S.rowIndex = keep;
  drawTabs();
  await drawView();
}

// --- dialogs ----------------------------------------------------------------

function showModal(title, okLabel) {
  $('#modal-title').textContent = title;
  const ok = $('#modal-ok');
  if (okLabel === null) ok.style.display = 'none';
  else { ok.style.display = ''; ok.textContent = okLabel; }
  $('#modal').hidden = false;
  return $('#modal-body');
}

function openNewCampaign(source) {
  const body = showModal('New campaign', 'Create');
  body.textContent = '';

  const from = el('select', {}, S.info.campaigns.map(c =>
    el('option', {value: c.file, selected: c.file === (source || S.file)},
       c.file)));
  const name = el('input', {type: 'text', placeholder: 'Korea 1983',
                            spellcheck: 'false'});
  const ui = el('input', {type: 'text', placeholder: 'shown in the game UI',
                          spellcheck: 'false'});

  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'Start from'}), from]));
  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'File name'}), name]));
  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'Campaign title'}), ui]));
  body.appendChild(el('p', {class: 'note', text:
    'A campaign is only partly authored data — the objective graph, the ' +
    'terrain links, the team records and the weather all have to agree with ' +
    'each other, and the engine will not rebuild them from nothing. So a new ' +
    'campaign starts as a copy of one that already works, renamed. ' +
    'Everything in it is then editable here.'}));
  body.appendChild(el('p', {class: 'note', text:
    'The trigger script comes across as <name>.tri, so the copy ends the way ' +
    'its source does — change it on the Victory tab. Starting from a save ' +
    'also brings the scenario\u2019s objective list across, which is what the ' +
    'engine reads a save\u2019s objectives from.'}));

  $('#modal-ok').onclick = guard(async () => {
    const res = await api('/api/campaign/new', {
      theater: S.theater, source: from.value,
      name: name.value.trim(), uiName: ui.value.trim(),
    });
    $('#modal').hidden = true;
    toast('Created ' + res.file + ' from ' + res.from +
          (res.script ? '  \u00b7  ' + res.script : ''), 'ok');
    S.info = await api('/api/theater?theater=' + encodeURIComponent(S.theater));
    await loadState();
    await openCampaign(res.file);
  });
}

// A save's ending comes from the scenario its header names, so giving it one
// of its own means copying that scenario -- objectives included -- and
// pointing the save at the copy. See api_save_ending for the engine's rules.
function openSaveEnding(file) {
  const body = showModal('Give this save its own ending', 'Create');
  body.textContent = '';

  const stem = file.replace(/\.[^.]+$/, '');
  const name = el('input', {type: 'text', value: stem + ' ending',
                            spellcheck: 'false'});
  const ui = el('input', {type: 'text', placeholder: 'shown in the game UI',
                          spellcheck: 'false'});

  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'New scenario name'}), name]));
  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'Campaign title'}), ui]));
  body.appendChild(el('p', {class: 'note', text:
    'A save has no trigger script of its own: the engine reads the one named ' +
    'by the Scenario field in its header. This makes a copy of that scenario ' +
    'with the same objective list, writes <name>.tri for it, and points this ' +
    'save at the copy \u2014 so you can edit the ending here without changing ' +
    'the scenario it came from. Units, teams, pilots and the objective ' +
    'deltas still come from the save.'}));

  $('#modal-ok').onclick = guard(async () => {
    const res = await api('/api/save/ending', {
      theater: S.theater, file: file,
      name: name.value.trim(), uiName: ui.value.trim(),
    });
    $('#modal').hidden = true;
    toast('Created ' + res.scenario +
          (res.script ? ' and ' + res.script : ''), 'ok');
    await refreshPending();
    await drawView();
  });
}

function openNewTheater(baseTdf) {  const body = showModal('New theater', 'Create');
  body.textContent = '';
  const name = el('input', {type: 'text', placeholder: 'Korea 1980s',
                            spellcheck: 'false'});
  const desc = el('input', {type: 'text',
    placeholder: 'Allies v. DPRK, 1983 inventory', spellcheck: 'false'});
  const folder = el('input', {type: 'text', placeholder: 'auto from the name',
                              spellcheck: 'false'});
  const base = el('select', {}, S.theaters.map(t =>
    el('option', {value: t.file, selected: t.file === baseTdf}, t.name)));
  const copyArt = el('input', {type: 'checkbox'});

  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'Clone from'}), base]));
  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'Theater name'}), name]));
  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'Description'}), desc]));
  body.appendChild(el('label', {class: 'field'},
                      [el('span', {text: 'Campaign folder'}), folder]));
  body.appendChild(el('label', {class: 'checkline'}, [copyArt,
    el('span', {text: 'Also copy the art directory (slower; only needed if ' +
                      'you will change the UI art)'})]));
  body.appendChild(el('p', {class: 'note', text:
    'This copies the whole campaign folder, including its CampaignDB. ' +
    'Terrain and objects stay shared with the source theater.'}));

  $('#modal-ok').onclick = guard(async () => {
    const btn = $('#modal-ok');
    btn.disabled = true;
    btn.textContent = 'Copying…';
    try {
      const res = await api('/api/theater/create', {
        base: base.value, name: name.value.trim(), desc: desc.value.trim(),
        folder: folder.value.trim(), copyArt: copyArt.checked,
      });
      $('#modal').hidden = true;
      toast('Created ' + res.theater.name + ' — ' +
            res.theater.files_copied + ' files, ' +
            (res.theater.bytes / 1048576).toFixed(1) + ' MB', 'ok');
      await loadState();
      await selectTheater(res.theater.tdf);
    } finally {
      btn.disabled = false;
      btn.textContent = 'Create';
    }
  });
}

async function deleteTheater(tdf, name) {
  const body = showModal('Delete theater', 'Delete');
  body.textContent = '';
  const removeCampaign = el('input', {type: 'checkbox'});
  body.appendChild(el('p', {text: 'Remove "' + name +
    '" from theater.lst and delete its .tdf?'}));
  body.appendChild(el('label', {class: 'checkline'}, [removeCampaign,
    el('span', {text: 'Also delete its campaign folder and everything in it'})]));
  body.appendChild(el('p', {class: 'note warn', text: 'This cannot be undone.'}));
  $('#modal-ok').onclick = guard(async () => {
    const res = await api('/api/theater/delete',
                          {tdf: tdf, removeCampaign: removeCampaign.checked});
    $('#modal').hidden = true;
    toast('Removed ' + res.removed.join(', '), 'ok');
    S.theater = null; S.info = null; S.tab = null; S.file = null;
    await loadState();
    drawTabs();
    await drawView();
  });
}

// --- boot -------------------------------------------------------------------

$('#btn-new-theater').onclick =
  () => openNewTheater(S.theater || (S.theaters[0] || {}).file);
$('#modal-cancel').onclick = () => { $('#modal').hidden = true; };
$('#modal').onclick = e => { if (e.target.id === 'modal') $('#modal').hidden = true; };

$('#btn-gamedir').onclick = guard(async () => {
  await api('/api/gamedir', {gamedir: $('#gamedir-input').value.trim()});
  S.theater = null; S.info = null; S.tab = null; S.file = null;
  await loadState();
  drawTabs();
  await drawView();
  toast('Game directory set', 'ok');
});

$('#btn-discard').onclick = guard(async () => {
  const res = await api('/api/discard', {});
  if (typeof mapReset === 'function') mapReset();
  toast('Discarded ' + res.discarded + ' pending change(s)', 'ok');
  if (S.theater) {
    S.info = await api('/api/theater?theater=' + encodeURIComponent(S.theater));
  }
  await loadState();
  drawTabs();
  await drawView();
});

$('#btn-save').onclick = guard(async () => {
  $('#btn-save').disabled = true;
  const res = await api('/api/save', {});
  toast(res.written.length
    ? 'Wrote ' + res.written.length + ' file(s): ' + res.written.join(', ') +
      (res.backups.length ? '   ·   backup in ' + res.backups.join(', ') : '')
    : 'Nothing to write', 'ok');
  if (S.theater) {
    S.info = await api('/api/theater?theater=' + encodeURIComponent(S.theater));
  }
  await loadState();
  drawTabs();
  await drawView();
});

window.addEventListener('beforeunload', e => {
  if (S.pending.length) { e.preventDefault(); e.returnValue = ''; }
});

guard(loadState)();
