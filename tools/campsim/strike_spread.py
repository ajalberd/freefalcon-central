"""Can a player's deep strikes on the North's economy change a 10-day Korea war?  Delayed-strike harness against a no-strike control.

    python strike_spread.py run [seeds=12]     queue every arm (arms x seeds) through supply_spread.run_one
    python strike_spread.py report             the table, from strike/results.jsonl

Arms are (mechanism) x (strike / control), on the same seeds.  Everything is campsim only; the mechanisms are off by default in the engine.
  strike   factories, power/nuclear plants and refineries of the North destroyed from hour 48 and held down (blow=...:6:hold, blowat=48)
  ai       the same targets as sliders instead (objpri=prod/pol/depot/roads at 100): Blue's own AI flies the strikes, so Blue pays for them
  M1       starvation drains morale (starvemorale=12:30): below half supply a battalion loses morale, and breaks under 30
  M2       supply-aware fall-back (supplyfall=1): a battalion under half supply goes to the nearest supply source behind the third line
  M3       stronger SAMs (make_sam_mod.py): an SA-10 and an SA-5 battalion on each of the 74 factory / power / refinery sites, the Chinese AD all SA-10
Base for every arm: the 86-battalion China staged on its own side of the Yalu (chnb), the stock win rule (no gates, no timers), the supply knobs
noprod=1 prodsup=0.75 usemult=2 (the best of the supply spread), the China counteroffensive at 8 allied battalions.
"""
import json
import math
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import supply_spread as S  # noqa: E402

S.OUT = os.path.join(HERE, "strike")
S.RES = os.path.join(S.OUT, "results.jsonl")
S.STAGE.update({"sb": "chnb", "sx": "chboth", "ss": "chsite", "sc": "chchn", "s5": "chsa5", "sd": "chdum"})

K = "noprod=1 prodsup=0.75 usemult=2"
STRIKE = "blow=factory:6:hold blow=power:6:hold blow=refinery:6:hold blowat=48"
AI = "objpri=prod:100,pol:100,depot:100,roads:60"
M1 = "starvemorale=12:30"
M2 = "supplyfall=1"

# arm -> (stage, knobs)
ARMS = {
    "ctl": ("sb", K), "str": ("sb", K + " " + STRIKE),
    "ctl_m1": ("sb", K + " " + M1), "str_m1": ("sb", K + " " + STRIKE + " " + M1),
    "ctl_m2": ("sb", K + " " + M2), "str_m2": ("sb", K + " " + STRIKE + " " + M2),
    "ctl_m12": ("sb", K + " " + M1 + " " + M2), "str_m12": ("sb", K + " " + STRIKE + " " + M1 + " " + M2),
    "ctl_sam": ("sx", K), "str_sam": ("sx", K + " " + STRIKE),
    "ctl_all": ("sx", K + " " + M1 + " " + M2), "str_all": ("sx", K + " " + STRIKE + " " + M1 + " " + M2),
    "ai": ("sb", K + " " + AI), "ai_sam": ("sx", K + " " + AI),
}
# second round: other ways to pull the same levers (8 seeds each)
ST = "stocks=1 storecap=25 storestart=50"          # M4: depots and army bases are stores with a local stock (the store model; units with no store in reach get nothing)
M1H = "starvemorale=30:45"                          # a harsher M1
SHOCK = "strikeshock=20:60"                         # M5: morale falls while the unit's own supply / production / airbase / bridge objectives within 60 km are damaged
INIT = "initsupply=1"                               # M6: the initiative score counts supply-weighted ground vehicles (a starved army gives the other side the initiative)
IW = "initw=8:1:1"                                  # M7: the initiative blend weighted to the ground war (stock 5:4:1), so China's aircraft do not decide who attacks
HOLD = "holdtest=12 holdat=84 holdchain=1 playerhold=1"   # a 'player': from hour 84 (Blue is defensive) 12 Blue battalions are sent at the nearest North objective, and on to the next when it falls
HOLD0 = "holdtest=12 holdat=84 holdchain=1 playerhold=0"   # the same orders, but the AI may take the battalions back (stock)
AB = "blow=airbase:6:hold"                          # the North's airbases held destroyed too: the team aircraft statistic is squadron strength x home airbase status
ABR = "blow=airbase:red:hold"                       # ... including China's (teams 4, 5, 6)
CI0 = "counterinit=0"                                # the cfg's CounterAttackInitiative 15 off: stock thresholds (50 to start, 55 for a full offensive)
AC = "alliedcounter=1:8:80:%d"                      # the China counteroffensive with a rest of N hours between its strikes (stock tests used no rest)
HARD = "noprod=1 prodsup=0.5 usemult=4 " + M1H + " " + M2
EXTRA = {
    "ctl_st": ("sb", K + " " + ST), "str_st": ("sb", K + " " + STRIKE + " " + ST),
    "str24": ("sb", K + " " + STRIKE.replace("blowat=48", "blowat=24")), "str96": ("sb", K + " " + STRIKE.replace("blowat=48", "blowat=96")),
    "strF": ("sb", K + " blow=factory:6:hold blowat=48"), "strP": ("sb", K + " blow=power:6:hold blowat=48"),
    "ctl_dum": ("sd", K), "str_dum": ("sd", K + " " + STRIKE),         # control for M3: 148 extra weak (KS-19 gun) battalions on the same sites
    "ctl_site": ("ss", K), "str_site": ("ss", K + " " + STRIKE),      # M3 split: the 148 site battalions only
    "ctl_chn": ("sc", K), "str_chn": ("sc", K + " " + STRIKE),         # M3 split: the Chinese AD only
    "ctl_sh": ("sb", K + " " + SHOCK), "str_sh": ("sb", K + " " + STRIKE + " " + SHOCK),
    "ctl_sh12": ("sb", K + " " + SHOCK + " " + M1 + " " + M2), "str_sh12": ("sb", K + " " + STRIKE + " " + SHOCK + " " + M1 + " " + M2),
    "ctl_is": ("sb", K + " " + INIT), "str_is": ("sb", K + " " + STRIKE + " " + INIT),
    "ctl_is12": ("sb", K + " " + INIT + " " + M1 + " " + M2), "str_is12": ("sb", K + " " + STRIKE + " " + INIT + " " + M1 + " " + M2),
    "ctl_iw": ("sb", K + " " + IW), "str_iw": ("sb", K + " " + STRIKE + " " + IW),
    "ctl_hold": ("sb", K + " " + HOLD), "str_hold": ("sb", K + " " + STRIKE + " " + HOLD),
    "ctl_hold0": ("sb", K + " " + HOLD0), "str_m12_hold": ("sb", K + " " + STRIKE + " " + M1 + " " + M2 + " " + HOLD),
    "str_ab": ("sb", K + " " + STRIKE + " " + AB), "str_abr": ("sb", K + " " + STRIKE + " " + ABR),
    "str_ab_ci0": ("sb", K + " " + STRIKE + " " + AB + " " + CI0), "str_ab_ci0_nac": ("sb", K + " " + STRIKE + " " + AB + " " + CI0 + " alliedcounter=0"),
    "str_ci0": ("sb", K + " " + STRIKE + " " + CI0), "ctl_ci0": ("sb", K + " " + CI0),
    "str_ab_r12": ("sb", K + " " + STRIKE + " " + AB + " " + AC % 12), "str_ab_r24": ("sb", K + " " + STRIKE + " " + AB + " " + AC % 24),
    "str_r12": ("sb", K + " " + STRIKE + " " + AC % 12), "str_r24": ("sb", K + " " + STRIKE + " " + AC % 24),
    "ctl_hard": ("sb", HARD), "str_hard": ("sb", HARD + " " + STRIKE),          # a tight supply regime with M1 (harsh) and M2: use x4, production 0.5
    "ctl_hsam": ("sx", HARD), "str_hsam": ("sx", HARD + " " + STRIKE),          # ... and the SAM world
    "ctl_m12h": ("sb", K + " " + M1H + " " + M2), "str_m12h": ("sb", K + " " + STRIKE + " " + M1H + " " + M2),
}
ARMS.update(EXTRA)
for name, (stage, knobs) in ARMS.items():
    S.CONFIGS["x_" + name] = knobs
ORDER2 = ["ctl_dum", "str_dum", "ctl_site", "str_site", "ctl_chn", "str_chn", "ctl_st", "str_st", "str24", "str96", "strF", "strP", "ctl_m12h", "str_m12h"]
ORDER = ["ctl", "str", "ctl_sam", "str_sam", "ctl_m12", "str_m12", "ctl_all", "str_all", "ai", "ai_sam", "ctl_m1", "str_m1", "ctl_m2", "str_m2"]

_orig_analyse = S.analyse


def analyse(stage, cfg, seed):
    m = _orig_analyse(stage, cfg, seed)
    base = os.path.join(HERE, "runs", S.runname(stage, cfg, seed))
    fr = [json.loads(l) for l in open(base + ".jsonl") if l.startswith('{"t"')]
    log = open(base + ".log", errors="replace").read()
    for h in (24, 48, 96, 120, 192):
        f = S.hour(fr, h)
        m["h%d" % h] = {"obj_d": f["s"][6][4], "obj_b": f["s"][2][4], "veh_d": f["s"][6][0], "veh_b": f["s"][2][0],
                        "sup_d": f["sp"][6][30], "front_d": f["sp"][6][32], "low_d": f["sp"][6][33], "prod_d": f["sp"][6][21]}
    m["ser"] = [[h, S.hour(fr, h)["s"][2][4], S.hour(fr, h)["s"][6][4], S.hour(fr, h)["sp"][6][32]] for h in range(0, 241, 12)]
    lost = {}
    for t, a, g in re.findall(r"LOSS h=\d+ team (\d): .*?\| air<-air (\d+)\s+<-ground (\d+)", log):
        lost[int(t)] = (int(a), int(g))          # the last line wins: cumulative
    m["blue_air_lost"] = sum(lost.get(t, (0, 0))[0] + lost.get(t, (0, 0))[1] for t in (1, 2))
    m["blue_air_lost_gnd"] = sum(lost.get(t, (0, 0))[1] for t in (1, 2))
    m["red_air_lost_gnd"] = lost.get(6, (0, 0))[1]
    depth = {}
    ad = {}
    for t, body in re.findall(r"AIRLOSSDEPTH h=\d+ team (\d) [^:]*\):(.*)", log):
        # keep the last printed line per team
        depth[int(t)] = {int(y): (int(a), int(b)) for y, a, b in re.findall(r" y(\d+):(\d+)/(\d+)", body)}
    for t in (1, 2):
        for y, (a, b) in depth.get(t, {}).items():
            d = max(0, y - 490)                  # kilometres north of the starting front, in 50 km bands
            ad.setdefault(d, [0, 0])
            ad[d][0] += a
            ad[d][1] += b
    m["gnd_loss_depth"] = {str(k): v for k, v in sorted(ad.items())}
    sf = re.findall(r"SUPPLYFALL h=\d+ starved battalions sent to a supply source: (\d+)", log)
    m["fall_moves"] = int(sf[-1]) if sf else 0
    m["blow_log"] = re.findall(r"BLOW type \d+ team \d.*", log)[:1]
    ev = {int(a): int(c) / 60.0 for a, c in re.findall(r"EVENT (\d+) FIRED at min (\d+)", log)}
    late = [x for x in fr if x["t"] / 60 >= 72]
    m["cap_d"] = round(sum(sum(1 for u in x["u"] if u[2] == 6 and u[1] == 0 and u[9] == 1) for x in late) / float(max(1, len(late))), 1)
    m["cap_b"] = round(sum(sum(1 for u in x["u"] if u[2] == 2 and u[1] == 0 and u[9] == 1) for x in late) / float(max(1, len(late))), 1)
    m["mor_d"] = round(sum(sum(u[8] for u in x["u"] if u[2] == 6 and u[1] == 0) / float(max(1, sum(1 for u in x["u"] if u[2] == 6 and u[1] == 0))) for x in late) / float(max(1, len(late))), 1)
    hc = re.findall(r"HOLDCAP h=\d+ held battalions ordered (\d+) times, objectives taken (\d+)", log)
    m["hold_orders"], m["hold_taken"] = (int(hc[-1][0]), int(hc[-1][1])) if hc else (0, 0)
    m["red_win"] = ev.get(14)             # Seoul or Pusan fell
    m["stalemate"] = ev.get(15)
    return m


S.analyse = analyse


def run(seeds, order=None):
    items = []
    for s in range(1, seeds + 1):
        for arm in order or ORDER:
            stage, _ = ARMS[arm]
            items.append((stage, "x_" + arm, s))
    S.queue(items)


# ---------------------------------------------------------------- the plan after the first 46 runs

PLAN3 = {"ctl_hard": 6, "str_hard": 6, "ctl_hsam": 6, "str_hsam": 6}
PLAN4 = {"ctl_st": 6, "str_st": 6, "str24": 6, "str96": 6, "strF": 6, "strP": 6}
PLAN8 = {a: 12 for a in ("ctl_sh", "str_sh", "ctl_sh12", "str_sh12")}      # M5 strike shock, 12 seeds
PLAN7 = {a: 8 for a in ("ctl_is", "str_is", "ctl_is12", "str_is12")}      # M6 initiative with supply, 8 seeds
PLAN10 = {a: 8 for a in ("ctl_iw", "str_iw")}      # M7 ground-weighted initiative, 8 seeds
PLAN11 = {a: 6 for a in ("ctl_hold", "str_hold", "ctl_hold0", "str_m12_hold")}      # a player pushing Blue north from hour 84
PLAN12 = {"str_ab": 3, "str_abr": 3}      # airbase strikes: does the aircraft term of initiative move?
PLAN13 = {"str_ab_ci0": 3, "str_ab_ci0_nac": 3, "str_ci0": 3, "ctl_ci0": 3}      # is the 15-point counterattack setting what locks Blue out?
PLAN14 = {"str_ab_r12": 3, "str_ab_r24": 3, "str_r12": 3, "str_r24": 3}      # a rest between the allied strikes lets Blue's turn come round
PLAN5 = {a: 12 for a in ("ctl_m12", "str_m12", "ctl_all", "str_all", "ai", "ai_sam", "ctl_m1", "str_m1", "ctl_m2", "str_m2", "ctl_st", "str_st",
                         "ctl_hard", "str_hard", "ctl_hsam", "str_hsam")}      # top-up: every arm in the headline tables to 12 seeds
PLAN6 = {a: 12 for a in ("str24", "str96", "strF", "strP")}      # timing / target-class arms to 12 seeds
PLAN9 = {a: 12 for a in ("ctl", "str", "ctl_sam", "str_sam", "ctl_dum", "str_dum", "ctl_m12", "str_m12", "ctl_all", "str_all", "ai", "ai_sam", "ctl_m1", "str_m1",
                         "ctl_m2", "str_m2", "ctl_st", "str_st", "ctl_hard", "str_hard", "ctl_hsam", "str_hsam", "ctl_sh", "str_sh", "ctl_sh12", "str_sh12",
                         "ctl_is", "str_is", "ctl_is12", "str_is12", "str24", "str96", "strF", "strP")}      # every arm with results, to 12 seeds
PLAN = {          # arm -> seeds to reach
    "ctl": 12, "str": 12, "ctl_sam": 12, "str_sam": 12, "ctl_dum": 12, "str_dum": 12,
    "ctl_all": 8, "str_all": 8, "ctl_m12": 8, "str_m12": 8, "ai": 8, "ai_sam": 8,
    "ctl_m1": 4, "str_m1": 4, "ctl_m2": 4, "str_m2": 4,
}
import threading as _th
_launch = _th.Lock()


def campsim_count():
    import subprocess
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq campsim.exe", "/NH"], stdout=subprocess.PIPE, text=True).stdout
    return sum(1 for l in out.splitlines() if l.lower().startswith("campsim"))


def paced_run_one(stage, cfg, seed):
    """Wait for a free core slot (other queues may still be running), one launch a minute, then run."""
    with S.lock:
        record()                                                    # finished runs that nobody recorded
        done = any(o["stage"] == stage and o["cfg"] == cfg and o["seed"] == seed and o["hours"] >= 238 for o in S.load())
    if done:
        return None
    log = os.path.join(HERE, "runs", S.runname(stage, cfg, seed) + ".log")
    if os.path.exists(log) and time.time() - os.path.getmtime(log) < 20 * 60:
        return None                                                 # still running from an earlier queue
    with _launch:
        while campsim_count() >= S.PAR:
            time.sleep(30)
        time.sleep(75)                       # let the campsim just launched show up in the count
    return S.run_one(stage, cfg, seed)


def run_plan(plan=None):
    items = []
    plan = plan or PLAN
    top = max(plan.values())
    for sd in range(1, top + 1):
        for arm, n in plan.items():
            if sd <= n:
                items.append((ARMS[arm][0], "x_" + arm, sd))
    # runs already in flight from the first queue are recorded by `record`; the pacing keeps the machine at S.PAR campsims
    from concurrent.futures import ThreadPoolExecutor
    print("plan: %d runs" % len(items), flush=True)
    with ThreadPoolExecutor(max_workers=S.PAR) as ex:
        list(ex.map(lambda a: paced_run_one(*a), items))


def reanalyse():
    """Recompute every x_ run's numbers from its files (after a change to analyse) and rewrite results.jsonl for them."""
    keep = [r for r in S.load() if not r["cfg"].startswith("x_")]
    out = []
    for r in load():
        n = analyse(r["stage"], r["cfg"], r["seed"])
        n["score"] = S.score(n)
        n["wall_s"] = r.get("wall_s", 0)
        out.append(n)
    with S.lock:
        open(S.RES, "w").write("".join(json.dumps(o) + "\n" for o in out))
    print("reanalysed", len(out))


def record():
    """Analyse finished x_ runs that no queue recorded (their orchestrator was stopped)."""
    import glob
    have = {(r["stage"], r["cfg"], r["seed"]) for r in S.load()}
    for f in sorted(glob.glob(os.path.join(HERE, "runs", "*-sp-x_*-s*.log"))):
        m = re.match(r"(\w+)-sp-(x_\w+)-s(\d+)\.log$", os.path.basename(f))
        if not m or "RESULT" not in open(f, errors="replace").read()[-600:]:
            continue
        save, cfg, seed = m.group(1), m.group(2), int(m.group(3))
        stage = [k for k, v in S.STAGE.items() if v == save and k in ("sb", "sx", "ss", "sc", "s5", "sd")][0]
        if (stage, cfg, seed) in have:
            continue
        r = analyse(stage, cfg, seed)
        r["score"] = S.score(r)
        r["wall_s"] = 0
        open(S.RES, "a").write(json.dumps(r) + "\n")
        print("recorded", save, cfg, seed)


# ---------------------------------------------------------------- report

def load():
    seen = {}
    for r in S.load():                       # the last record of a (stage, arm, seed) wins
        if r["cfg"].startswith("x_"):
            seen[(r["stage"], r["cfg"], r["seed"])] = r
    return list(seen.values())


def mean_se(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return None, None
    mu = sum(xs) / len(xs)
    sd = math.sqrt(sum((x - mu) ** 2 for x in xs) / max(1, len(xs) - 1)) if len(xs) > 1 else 0.0
    return mu, sd / math.sqrt(len(xs))


def share(r, h="h240"):
    f = r[h]
    return 100.0 * f["obj_b"] / max(1, f["obj_b"] + f["obj_d"])


def report():
    rows = load()
    by = {}
    for r in rows:
        by.setdefault(r["cfg"][2:], []).append(r)
    cols = [("Blue share h144 %", lambda r: share(r, "h144")), ("Blue share h240 %", share), ("North front supply min %", lambda r: r["front_min"]),
            ("North front supply h240", lambda r: r["h240"]["front_d"]), ("North bn <25% h240", lambda r: r["h240"]["low_d"]),
            ("Blue aircraft lost", lambda r: r.get("blue_air_lost")), ("...of which to ground fire", lambda r: r.get("blue_air_lost_gnd")),
            ("Blue win hour", lambda r: r["blue_win"]), ("Blue won (0/1)", lambda r: 1.0 if r["blue_win"] is not None else 0.0),
            ("starved bn sent back", lambda r: r.get("fall_moves")),
            ("North bn on CAPTURE (h72+)", lambda r: r.get("cap_d")), ("Blue bn on CAPTURE (h72+)", lambda r: r.get("cap_b")),
            ("North mean morale (h72+)", lambda r: r.get("mor_d")), ("North won (event 14)", lambda r: 1.0 if r.get("red_win") is not None else 0.0)]
    print("%-10s %3s " % ("arm", "n") + " ".join("%-24s" % c[0] for c in cols))
    for arm in ORDER + ORDER2 + ["ctl_hard", "str_hard", "ctl_hsam", "str_hsam", "ctl_sh", "str_sh", "ctl_sh12", "str_sh12", "ctl_is", "str_is", "ctl_is12", "str_is12", "ctl_iw", "str_iw"]:
        rs = by.get(arm)
        if not rs:
            continue
        cells = []
        for _, fn in cols:
            mu, se = mean_se([fn(r) for r in rs])
            cells.append("%-24s" % ("-" if mu is None else "%.1f +-%.1f" % (mu, se)))
        print("%-10s %3d " % (arm, len(rs)) + " ".join(cells))
    print()
    print("Blue aircraft lost to ground fire by how far north of the starting front (y 490) the shooter stood; mean per run, any shooter / air-defence battalion:")
    bands = sorted({int(k) for r in rows for k in r.get("gnd_loss_depth", {})})
    print("%-10s %3s " % ("arm", "n") + " ".join("%9s" % ("%d+" % b) for b in bands))
    for arm in ("ctl", "str", "ctl_sam", "str_sam", "ctl_dum", "str_dum", "ai", "ai_sam"):
        rs = by.get(arm)
        if not rs:
            continue
        cells = []
        for b in bands:
            a = sum(r.get("gnd_loss_depth", {}).get(str(b), [0, 0])[0] for r in rs) / float(len(rs))
            d = sum(r.get("gnd_loss_depth", {}).get(str(b), [0, 0])[1] for r in rs) / float(len(rs))
            cells.append("%4.0f/%-4.0f" % (a, d))
        print("%-10s %3d " % (arm, len(rs)) + " ".join(cells))
    print()
    print("strike minus control, same seeds (paired), Blue share at h240 (points) and Blue win hour:")
    for mech in ("", "_m1", "_m2", "_m12", "_sam", "_all", "_dum", "_hard", "_hsam", "_sh", "_sh12", "_is", "_is12", "_iw", "_site", "_chn", "_st", "_m12h"):
        a, b = by.get("str" + mech), by.get("ctl" + mech)
        if not a or not b:
            continue
        sa = {r["seed"]: r for r in a}
        sb = {r["seed"]: r for r in b}
        common = sorted(set(sa) & set(sb))
        d = [share(sa[s]) - share(sb[s]) for s in common]
        mu, se = mean_se(d)
        wa = [sa[s]["blue_win"] for s in common]
        wb = [sb[s]["blue_win"] for s in common]
        print("  %-6s n=%2d  share %+5.1f +-%.1f   wins %d vs %d   mean win hour %s vs %s" % (
            mech or "base", len(common), mu, se, sum(1 for w in wa if w is not None), sum(1 for w in wb if w is not None),
            "%.0f" % (sum(w for w in wa if w is not None) / max(1, sum(1 for w in wa if w is not None))) if any(w is not None for w in wa) else "-",
            "%.0f" % (sum(w for w in wb if w is not None) / max(1, sum(1 for w in wb if w is not None))) if any(w is not None for w in wb) else "-"))
    print()
    print("when and what to strike (base world), paired against the no-strike control on the same seeds:")
    for arm, label in (("str24", "all three from hour 24"), ("str", "all three from hour 48"), ("str96", "all three from hour 96"),
                       ("strF", "factories only, from 48"), ("strP", "power / nuclear plants only, from 48")):
        a, b = by.get(arm), by.get("ctl")
        if not a or not b:
            continue
        sb = {r["seed"]: r for r in b}
        d = [share(r) - share(sb[r["seed"]]) for r in a if r["seed"] in sb]
        fs = [r["front_min"] for r in a]
        cd = [r.get("cap_d") for r in a if r.get("cap_d") is not None]
        mu, se = mean_se(d)
        print("  %-7s %-38s n=%2d  share %+5.1f +-%.1f   North front supply low %.0f%%   North bn on CAPTURE %s" % (
            arm, label, len(d), mu, se, sum(fs) / len(fs), "%.1f" % (sum(cd) / len(cd)) if cd else "-"))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "report"
    os.makedirs(S.OUT, exist_ok=True)
    if cmd == "run":
        run(int(sys.argv[2]) if len(sys.argv) > 2 else 12)
    elif cmd == "plan":
        run_plan()
    elif cmd == "plan3":
        run_plan(PLAN3)
    elif cmd == "plan14":
        run_plan(PLAN14)
    elif cmd == "plan13":
        run_plan(PLAN13)
    elif cmd == "plan12":
        run_plan(PLAN12)
    elif cmd == "plan11":
        run_plan(PLAN11)
    elif cmd == "plan10":
        run_plan(PLAN10)
    elif cmd == "plan9":
        run_plan(PLAN9)
    elif cmd == "plan8":
        run_plan(PLAN8)
    elif cmd == "plan7":
        run_plan(PLAN7)
    elif cmd == "plan6":
        run_plan(PLAN6)
    elif cmd == "plan5":
        run_plan(PLAN5)
    elif cmd == "plan4":
        run_plan(PLAN4)
    elif cmd == "record":
        record()
    elif cmd == "reanalyse":
        reanalyse()
    elif cmd == "run2":
        run(int(sys.argv[2]) if len(sys.argv) > 2 else 8, ORDER2)
    else:
        report()
