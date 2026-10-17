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

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import supply_spread as S  # noqa: E402

S.OUT = os.path.join(HERE, "strike")
S.RES = os.path.join(S.OUT, "results.jsonl")
S.STAGE.update({"sb": "chnb", "sx": "chboth", "ss": "chsite", "sc": "chchn", "s5": "chsa5"})

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
EXTRA = {
    "ctl_st": ("sb", K + " " + ST), "str_st": ("sb", K + " " + STRIKE + " " + ST),
    "str24": ("sb", K + " " + STRIKE.replace("blowat=48", "blowat=24")), "str96": ("sb", K + " " + STRIKE.replace("blowat=48", "blowat=96")),
    "strF": ("sb", K + " blow=factory:6:hold blowat=48"), "strP": ("sb", K + " blow=power:6:hold blowat=48"),
    "ctl_m12h": ("sb", K + " " + M1H + " " + M2), "str_m12h": ("sb", K + " " + STRIKE + " " + M1H + " " + M2),
}
ARMS.update(EXTRA)
for name, (stage, knobs) in ARMS.items():
    S.CONFIGS["x_" + name] = knobs
ORDER2 = list(EXTRA)
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
    return m


S.analyse = analyse


def run(seeds, order=None):
    items = []
    for s in range(1, seeds + 1):
        for arm in order or ORDER:
            stage, _ = ARMS[arm]
            items.append((stage, "x_" + arm, s))
    S.queue(items)


# ---------------------------------------------------------------- report

def load():
    return [r for r in S.load() if r["cfg"].startswith("x_")]


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
            ("starved bn sent back", lambda r: r.get("fall_moves"))]
    print("%-10s %3s " % ("arm", "n") + " ".join("%-24s" % c[0] for c in cols))
    for arm in ORDER + ORDER2:
        rs = by.get(arm)
        if not rs:
            continue
        cells = []
        for _, fn in cols:
            mu, se = mean_se([fn(r) for r in rs])
            cells.append("%-24s" % ("-" if mu is None else "%.1f +-%.1f" % (mu, se)))
        print("%-10s %3d " % (arm, len(rs)) + " ".join(cells))
    print()
    print("strike minus control, same seeds (paired), Blue share at h240 (points) and Blue win hour:")
    for mech in ("", "_m1", "_m2", "_m12", "_sam", "_all", "_st", "_m12h"):
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


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "report"
    os.makedirs(S.OUT, exist_ok=True)
    if cmd == "run":
        run(int(sys.argv[2]) if len(sys.argv) > 2 else 12)
    elif cmd == "run2":
        run(int(sys.argv[2]) if len(sys.argv) > 2 else 8, ORDER2)
    else:
        report()
