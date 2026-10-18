"""Supply research trials (2026-10-18): what supply mechanisms do to the h_bar war. Deterministic campsim (det=1) and startups launched one at a
time (simultaneous startups still race on something shared and part at hour 1), so a mechanism is compared with ITS OWN seed's baseline.

    python supply_trials.py run [round]       queue the arms of a round, 3 seeds each
    python supply_trials.py report [round]    paired table from supply/results.jsonl

Per run: the end (event@hour), Blue's share of objectives at the end and its peak, Blue's loss of objectives from the peak (a setback), China's
join hour, Blue's share of time on the offensive after the join and its mean initiative, Blue SPEARHEAD supply (mean supply of Blue battalions
within 40 km of Blue's northern edge) on days 3 / 5 / 7, the North's front supply the same way, and the SXDIAG line (sources out of reach)."""
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "supply")
RES = os.path.join(OUT, "results.jsonl")
PAR = int(os.environ.get("TRIAL_JOBS", "9"))
SEEDS = (1, 2, 3)
HBAR = ("usecfg=1 tri=chn_appr2 alliedcounter=1:8:80 noprod=1 prodsup=0.75 usemult=2 chinaown=1 neutralnouse=1 neutralhold=1 initindep=1 "
        "turnmode=4 teambar=1:50,2:50,3:50,5:30,6:15 det=1")
lock = threading.Lock()
launch = threading.Lock()

# arm -> (extra knobs, round)
ARMS = {
    "base": ("", 1),
    "base_rep": ("", 1),                      # the same again: must match "base" exactly
    "own": ("sxown=1", 1),                     # only sources the team holds
    "rear24": ("sxrear=24", 1),                # a captured source counts after 24 h
    "rear72": ("sxrear=72", 1),
    "home": ("sxrear=9999", 1),                # only sources held since the start
    "hops6": ("sxhops=6", 1),                  # reach: 6 links
    "hops10": ("sxhops=10", 1),
    "loss5": ("sxloss=5", 1),                  # every road node costs 5% (stock 2)
    "loss5r0": ("sxloss=5 sxrail=0", 1),       # ... but rail is free: rail as the artery
    "home_loss5r0": ("sxrear=9999 sxloss=5 sxrail=0", 1),
    # round 2 (det clock build): can anything starve a spearhead? extremes first
    "b2": ("", 2),
    "loss10": ("sxloss=10", 2),
    "loss10r1": ("sxloss=10 sxrail=1", 2),
    "home_h8_l10": ("sxrear=9999 sxhops=8 sxloss=10", 2),
    "use4": ("usemult=4", 2),
    "use4_home_l10": ("usemult=4 sxrear=9999 sxloss=10", 2),
    # round 3: reach with forward depots after a delay (setback, then recovery?), and the engaged / convoy-time mechanisms
    "b3": ("", 3),
    "r_home": ("sxrear=9999 sxhops=8 sxloss=10", 3),
    "r48": ("sxrear=48 sxhops=8 sxloss=10", 3),
    "r72": ("sxrear=72 sxhops=8 sxloss=10", 3),
    "r120": ("sxrear=120 sxhops=8 sxloss=10", 3),
    "r72h12": ("sxrear=72 sxhops=12 sxloss=10", 3),
    "r72l5": ("sxrear=72 sxhops=8 sxloss=5", 3),
    "front0": ("sxfront=0", 3),
    "front25": ("sxfront=25", 3),
    "delay5": ("sxdelay=5", 3),
    "delay10": ("sxdelay=10", 3),
}


def runname(arm, seed):
    return "chdd-sx-%s-s%d" % (arm, seed)


def frames(path):
    out = []
    for l in open(path):
        if l.startswith('{"t"'):
            out.append(json.loads(l))
    return out


def edge_supply(f, teams, north):
    """mean supply of the team's battalions within 40 km of its forward edge (Blue: the northernmost; the North: the southernmost)"""
    bns = [u for u in f["u"] if u[2] in teams and u[1] == 0]
    if not bns:
        return None
    ys = sorted(u[4] for u in bns)
    edge = sum(ys[-10:]) / len(ys[-10:]) if north else sum(ys[:10]) / len(ys[:10])
    near = [u[11] for u in bns if abs(u[4] - edge) <= 40]
    return round(sum(near) / len(near), 1) if near else None


def analyse(arm, seed):
    base = os.path.join(HERE, "runs", runname(arm, seed))
    fr = frames(base + ".jsonl")
    log = open(base + ".log", errors="replace").read()
    ev = {int(a): int(c) / 60.0 for a, c in re.findall(r"EVENT (\d+) FIRED at min (\d+)", log)}
    res = re.findall(r"RESULT seed=\d+ endgame=(\d+) engine=\d+ campaign_h=([\d.]+)", log)
    m = {"arm": arm, "seed": seed, "endgame": int(res[-1][0]) if res else None, "end_h": float(res[-1][1]) if res else round(fr[-1]["t"] / 60, 1)}
    share = lambda f: 100.0 * f["s"][2][4] / max(1, f["s"][2][4] + f["s"][6][4] + f["s"][5][4])
    m["share_end"] = round(share(fr[-1]), 1)
    peak = max(fr, key=lambda f: f["s"][2][4])
    after = [f for f in fr if f["t"] >= peak["t"]]
    m["blue_peak_objs"] = peak["s"][2][4]
    m["blue_peak_h"] = round(peak["t"] / 60)
    m["blue_drop_from_peak"] = peak["s"][2][4] - min(f["s"][2][4] for f in after)
    m["blue_objs_end"] = fr[-1]["s"][2][4]
    j = ev.get(11)
    m["china_in_h"] = round(j) if j else None
    if j:
        aj = [f for f in fr if f["t"] / 60 >= j]
        if aj:
            m["blue_off_after_join"] = round(100 * sum(1 for f in aj if f["a"][2][0] == 4) / len(aj))
            m["blue_init_after_join"] = round(sum(f["a"][2][3] for f in aj) / len(aj), 1)
    for d in (3, 5, 7):
        f = min(fr, key=lambda q: abs(q["t"] / 60 - d * 24))
        if abs(f["t"] / 60 - d * 24) < 2:
            m["blue_spear_sup_d%d" % d] = edge_supply(f, (1, 2, 3), True)
            m["north_front_sup_d%d" % d] = edge_supply(f, (5, 6), False)
            m["blue_low_sup_d%d" % d] = sum(1 for u in f["u"] if u[2] in (1, 2, 3) and u[1] == 0 and u[11] < 50)
    sx = re.findall(r"SXDIAG h=\d+ team (\d+) searches (\d+), no source in reach (\d+), refused (\d+), mean links ([\d.]+)", log)
    for t in ("2", "6"):
        last = [x for x in sx if x[0] == t]
        if last:
            m["sx_t%s" % t] = [int(last[-1][1]), int(last[-1][2]), int(last[-1][3]), float(last[-1][4])]
    m["sig"] = hashlib.md5(json.dumps(fr[-1]["u"]).encode()).hexdigest()[:8]
    return m


def run_one(arm, seed):
    with lock:
        if os.path.exists(RES) and any(json.loads(l)["arm"] == arm and json.loads(l)["seed"] == seed for l in open(RES)):
            return
    knobs = ARMS[arm][0]
    name = runname(arm, seed)
    logp = os.path.join(HERE, "runs", name + ".log")
    cmd = [sys.executable, os.path.join(HERE, "run_batch.py"), "--save", "chdd", "--days", "10", "--seeds", str(seed), "--jobs", "1",
           "--tag", "sx-" + arm, "--set"] + (HBAR + " " + knobs).split()
    t0 = time.time()
    with launch:  # one startup at a time: release once this run is in its tick loop
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=HERE)
        time.sleep(1)
        while p.poll() is None:
            try:
                if os.path.getmtime(logp) >= t0 and "campsim: save=" in open(logp, errors="replace").read():
                    break
            except OSError:
                pass
            time.sleep(0.5)
        time.sleep(1)
    out = p.communicate()[0]
    try:
        m = analyse(arm, seed)
    except Exception as ex:
        m = {"arm": arm, "seed": seed, "error": str(ex), "tail": out[-300:]}
    m["wall_s"] = round(time.time() - t0)
    with lock:
        os.makedirs(OUT, exist_ok=True)
        open(RES, "a").write(json.dumps(m) + "\n")
    print("done %-28s end %s h%s share %s" % (name, m.get("endgame"), m.get("end_h"), m.get("share_end", m.get("error"))), flush=True)


def load():
    seen = {}
    if os.path.exists(RES):
        for l in open(RES):
            r = json.loads(l)
            if "error" not in r:
                seen[(r["arm"], r["seed"])] = r
    return seen


def f0(v):
    return "-" if v is None else ("%.0f" % v if isinstance(v, (int, float)) else str(v))


def report(rnd=None):
    rows = load()
    print("arm            seed | end      share(d vs base) peak@h drop | China in, Blue off%/init after | Blue spearhead supply d3/d5/d7 (low<50 d5) | North front d3/d5/d7 | SX t2 [search, out of reach, refused, links]")
    for arm, (knobs, r) in ARMS.items():
        if rnd and r != rnd:
            continue
        for s in SEEDS:
            x = rows.get((arm, s))
            if not x:
                continue
            b = rows.get(("base", s))
            d = (x["share_end"] - b["share_end"]) if b else None
            same = " =base" if b and arm != "base" and b.get("sig") == x.get("sig") else ""
            print("%-14s %d | %s@%-4s %4.1f(%+5.1f) %4d@%-3d %3d | %3s, %3s/%4s | %s/%s/%s (%s) | %s/%s/%s | %s%s" % (
                arm, s, x.get("endgame"), f0(x.get("end_h")), x["share_end"], d if d is not None else 0, x["blue_peak_objs"], x["blue_peak_h"],
                x["blue_drop_from_peak"], f0(x.get("china_in_h")), f0(x.get("blue_off_after_join")), f0(x.get("blue_init_after_join")),
                f0(x.get("blue_spear_sup_d3")), f0(x.get("blue_spear_sup_d5")), f0(x.get("blue_spear_sup_d7")), f0(x.get("blue_low_sup_d5")),
                f0(x.get("north_front_sup_d3")), f0(x.get("north_front_sup_d5")), f0(x.get("north_front_sup_d7")), x.get("sx_t2", "-"), same))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "report"
    rnd = int(sys.argv[2]) if len(sys.argv) > 2 else None
    os.makedirs(OUT, exist_ok=True)
    if cmd == "run":
        items = [(a, s) for s in SEEDS for a, (_, r) in ARMS.items() if not rnd or r == rnd]
        print("queue: %d runs, %d at a time" % (len(items), PAR), flush=True)
        with ThreadPoolExecutor(max_workers=PAR) as ex:
            list(ex.map(lambda a: run_one(*a), items))
    else:
        report(rnd)


if __name__ == "__main__":
    main()
