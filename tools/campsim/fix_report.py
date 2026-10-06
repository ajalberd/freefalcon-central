"""Compare campsim variants on the ground war: who wins, how far Blue gets, and whether orders and supply hold up.

    python fix_report.py fxbase fxkeep fxunits fxloss fxall [--save save0]

Reads runs/<save>-<tag>-s*.jsonl and .log. Win = event 17 (with tri=both: Pyongyang AND Wonsan).
"""
import argparse
import glob
import json
import math
import os
import re
import statistics

PY = (301, 580)
WIN_OBJS = {680, 260, 404}


def run_stats(jsonl):
    own, ev17, first_taken = {}, None, {}
    at = {}
    last = None
    for line in open(jsonl):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if 'meta' in d:
            for o in d['objs']:
                own[o[0]] = o[4]
            continue
        if 'u' not in d:
            continue
        h = d['t'] // 60
        for o in d.get('o', []):
            own[o[0]] = o[1]
            if o[0] in WIN_OBJS and o[1] == 2 and o[0] not in first_taken:
                first_taken[o[0]] = h
        if ev17 is None and 17 in d.get('ev', []):
            ev17 = h
        if h in (48, 96, 144) and h not in at:
            bl = [u for u in d['u'] if u[1] <= 1 and u[2] == 2]
            at[h] = {
                'objs': d['s'][2][4],
                'py_km': min(math.hypot(u[3] - PY[0], u[4] - PY[1]) for u in bl) if bl else 999,
                'dprk_veh': sum(u[5] for u in d['u'] if u[1] <= 1 and u[2] == 6),
            }
        last = d
    r = {'win': ev17, 'wonsan': first_taken.get(404), 'pyongyang': (max(first_taken[260], first_taken[680])
                                                                    if 260 in first_taken and 680 in first_taken else None),
         'at': at}
    sp = last.get('sp') if last else None
    if sp:
        s = sp[2]
        r['rok_resup'] = s[15]
        r['rok_empty'] = s[16]
        r['rok_front_sup'] = s[32]
    return r


def log_stats(log):
    r = {}
    if not os.path.exists(log):
        return r
    capord = suppath = None
    for line in open(log, errors='replace'):
        if line.startswith('CAPORD h=') and ' team 2:' in line:
            capord = line
        elif line.startswith('SUPPATH') and ' team 2:' in line:
            suppath = line
    if capord:
        m = re.search(r'assigned (\d+), ended (\d+) \(objective taken (\d+)\)', capord)
        if m:
            r['cap_assigned'], r['cap_taken'] = int(m.group(1)), int(m.group(3))
    if suppath:
        m = re.search(r'trips (\d+) .*no path (\d+), emptied on the road (\d+)', suppath)
        if m:
            r['trips'], r['nopath'], r['emptied'] = int(m.group(1)), int(m.group(2)), int(m.group(3))
    return r


def fmt_h(v):
    return f"h{v}" if v is not None else "-"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tags', nargs='+')
    ap.add_argument('--save', default='save0')
    a = ap.parse_args()
    rows = []
    for tag in a.tags:
        runs = sorted(glob.glob(f"runs/{a.save}-{tag}-s*.jsonl"))
        st = [dict(run_stats(f), **log_stats(f[:-6] + '.log')) for f in runs]
        if not st:
            continue
        n = len(st)
        wins = [s['win'] for s in st if s['win'] is not None]
        mean = lambda k, h=None: statistics.mean(
            [(s['at'][h][k] if h else s[k]) for s in st if (h in s['at'] if h else k in s)] or [0])
        rows.append((tag, [
            f"{len(wins)}/{n}" + (f" ({', '.join(fmt_h(w) for w in sorted(wins))})" if wins else ""),
            ', '.join(fmt_h(s['wonsan']) for s in st),
            ', '.join(fmt_h(s['pyongyang']) for s in st),
            f"{mean('objs', 48):.0f} / {mean('objs', 96):.0f} / {mean('objs', 144):.0f}",
            f"{mean('py_km', 96):.0f} / {mean('py_km', 144):.0f}",
            f"{mean('dprk_veh', 144):.0f}",
            f"{mean('cap_taken'):.0f} of {mean('cap_assigned'):.0f}",
            f"{100 * mean('rok_empty') / max(1, mean('rok_resup')):.0f}%",
            f"{mean('nopath'):.0f} / {mean('emptied'):.0f} of {mean('trips'):.0f}",
            f"{mean('rok_front_sup'):.0f}%",
        ]))
    head = ["variant", "Blue wins", "Wonsan falls", "Pyongyang falls", "Blue objs h48/96/144",
            "km to PY h96/144", "DPRK veh h144", "capture orders: taken of given", "ROK resupplies empty",
            "no path / emptied of trips", "ROK front bn supply"]
    for i, hd in enumerate(head[1:]):
        print(f"{hd:32s} " + " | ".join(f"{r[0]}: {r[1][i]}" for r in rows))


if __name__ == '__main__':
    main()
