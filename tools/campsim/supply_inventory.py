"""Supply inventory runs on Korea Rolling Fire (SUPPLY-INVENTORY.md section 9): what each burn / production scenario does to a side's supply.

    run_batch.py --game <gamework> --save save0 --days 10 --seeds 1 2 3 4 5 6 --jobs 6 --tag mod-<id> --set usecfg=1 noend=1 replfix=1
                 airliftneed=1 airliftrunway=1 [usemult=N useteam=T:M blow=factory:red:hold noprod=1 ...]

    python supply_inventory.py s0 s1 s2 ...        full run tags (mod-s0 ...); the first is the control

Reads runs/save0-<tag>-s<seed>.jsonl (the 'sp' rows, see main.cpp SupplyFrame) and .log (EVENT lines).  For each team and hour prints, as a
mean over the seeds: battalion supply % (all / within 30 km of the front / count under 25 %), the last distribution ratios (500 = the cap),
the pools, production capacity per day and what was produced / sent / received in the preceding 24 h.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "runs", "save0-%s-s%d.%s")
HRS = [24, 72, 120, 168, 239]
TEAMS = [(2, "ROK/Blue"), (6, "DPRK")]


def load(tag, seed):
    try:
        f = open(R % (tag, seed, "jsonl"))
    except OSError:
        return None
    json.loads(next(f))
    fr = [o for o in map(json.loads, f) if "t" in o]
    return fr or None


def at(fr, h):
    return min(fr, key=lambda x: abs(x["t"] / 60 - h))


def mean(v):
    v = list(v)
    return sum(v) / len(v) if v else 0.0


def events(tag, seed):
    out = {}
    try:
        for l in open(R % (tag, seed, "log"), errors="replace"):
            m = re.match(r"EVENT (\d+) FIRED at min (\d+)", l)
            if m:
                out.setdefault(int(m[1]), int(m[2]) / 60.0)
    except OSError:
        pass
    return out


def table(tags):
    """One row per tag: battalion supply % (Red / Blue at h24 72 120 168), battalions under 25 %, supply made a day, Blue win hour."""
    print("%-14s %5s  %-19s %-19s %5s %5s  %-11s %s" % ("tag", "seeds", "Red sup% 24/72/120/168", "Blue sup% 24/72/120/168", "R<25", "B<25", "made R/B", "Blue wins (mean hour)"))
    for tag in tags:
        runs = [fr for fr in (load(tag, s) for s in range(1, 13)) if fr]
        if not runs:
            print("%-14s no runs" % tag)
            continue
        wins = [events(tag, s).get(17) for s in range(1, 13) if load(tag, s)]
        won = [w for w in wins if w is not None]

        def sup(team, hs):
            return " ".join("%3.0f" % mean(at(f, h)["sp"][team][30] for f in runs) for h in hs)

        def made(team):
            return mean(at(f, 48)["sp"][team][8] - at(f, 24)["sp"][team][8] for f in runs)

        print("%-14s %5d  %-19s %-19s %5.0f %5.0f  %5.0f/%-5.0f %d/%d, %s" % (
            tag, len(runs), sup(6, (24, 72, 120, 168)), sup(2, (24, 72, 120, 168)),
            mean(at(f, 72)["sp"][6][33] for f in runs), mean(at(f, 168)["sp"][2][33] for f in runs),
            made(6), made(2), len(won), len(wins), "%.0f" % mean(won) if won else "-"))


def score(prefix):
    """Overnight goal (SUPPLY-INVENTORY.md section 13): one row per setting <prefix>-<id>-{none,bred,rblue}.
    c1 baseline wins and mean win hour; c2 Blue striking Red's plants: mean win hour vs baseline; c3 Red striking Blue's plants: mean win hour vs
    baseline and wins; c4 baseline battalion supply % days 5-7 (Red, Blue).  Win hours count only runs that won; wins are n / seeds."""
    import glob
    ids = sorted({os.path.basename(f)[len("save0-%s-" % prefix):].rsplit("-", 2)[0] for f in glob.glob(os.path.join(HERE, "runs", "save0-%s-*-none-s1.jsonl" % prefix))})

    def stats(tag):
        runs = [(s, load(tag, s)) for s in range(1, 13)]
        runs = [(s, fr) for s, fr in runs if fr]

        if not runs:
            return None

        wins = [events(tag, s).get(17) for s, _ in runs]
        won = [w for w in wins if w is not None]
        late = [mean(at(fr, h)["sp"][t][30] for fr in [f for _, f in runs] for h in (120, 144, 168)) for t in (6, 2)]
        return dict(n=len(runs), wins=len(won), hour=mean(won) if won else 0.0, red=late[0], blue=late[1], days=max(at(f, 9999)["t"] for _, f in runs) / 1440.0)

    print("%-12s %-4s | %-13s | %-26s | %-26s | %s" % ("setting", "days", "c1 base wins/h", "c2 Blue hits Red: wins/h (d)", "c3 Red hits Blue: wins/h (d)", "c4 Red / Blue supply d5-7"))

    for i in ids:
        b, r1, r2 = (stats("%s-%s-%s" % (prefix, i, k)) for k in ("none", "bred", "rblue"))

        if not (b and r1 and r2):
            print("%-12s incomplete" % i)
            continue

        def d(x):
            return "%+.0f%%" % (100.0 * (x["hour"] - b["hour"]) / b["hour"]) if b["hour"] and x["hour"] else "n/a"

        print("%-12s %3.0fd | %d/%d  %4.0f   | %d/%d  %4.0f (%s)%s | %d/%d  %4.0f (%s)%s | %3.0f / %3.0f" % (
            i, b["days"], b["wins"], b["n"], b["hour"], r1["wins"], r1["n"], r1["hour"], d(r1), "", r2["wins"], r2["n"], r2["hour"], d(r2), "", b["red"], b["blue"]))


FT = re.compile(r"FUELTANK h=(\d+) team (\d+): burned (\d+) points, (\d+) moves refused .*?; (\d+) wheeled/tracked battalions, mean tank (\d+)%, (\d+) empty")


def fuel(tags):
    """Fuel-tank runs (fueltank=1 / movefuel=N, SUPPLY-INVENTORY.md section 14): per tag and team, over the seeds and over h48-h168:
    mean tank %, empty tanks, moves refused per day, tank points burned per day, and the fuel pool (sp row 1) and its distribution ratio (row 6)."""
    print("%-12s %-5s %5s %6s %7s %8s %9s %8s %7s" % ("tag", "team", "seeds", "tank%", "empty", "refused/d", "burned/d", "fuelpool", "fratio"))
    for tag in tags:
        for team, name in TEAMS:
            rows, runs = [], []
            for s in range(1, 13):
                fr = load(tag, s)
                if not fr:
                    continue
                runs.append(fr)
                try:
                    for l in open(R % (tag, s, "log"), errors="replace"):
                        m = FT.match(l)
                        if m and int(m[2]) == team and 48 <= int(m[1]) <= 168:
                            rows.append([int(m[k]) for k in (3, 4, 6, 7)])
                except OSError:
                    pass
            if not runs:
                continue
            pool = mean(at(f, h)["sp"][team][1] for f in runs for h in (72, 120, 168))
            fra = mean(at(f, h)["sp"][team][6] for f in runs for h in (72, 120, 168))
            if rows:
                per = 4.0 * len(runs)  # 6-hour frames per day, per seed
                print("%-12s %-5s %5d %5.0f%% %7.1f %8.0f %9.0f %8.0f %7.0f" % (
                    tag, name[:5], len(runs), mean(r[2] for r in rows), mean(r[3] for r in rows),
                    sum(r[1] for r in rows) / (len(rows) / 4.0) , sum(r[0] for r in rows) / (len(rows) / 4.0), pool, fra))
            else:
                print("%-12s %-5s %5d %6s %7s %8s %9s %8.0f %7.0f" % (tag, name[:5], len(runs), "-", "-", "-", "-", pool, fra))


AF = re.compile(r"AIRFUEL h=(\d+) team (\d+): taken (\d+) returned (\d+) refilled (\d+), (\d+) launches with too little stock .*?; "
                r"(\d+) squadrons, (\d+) aircraft, mean stock (\d+) = (\d+) four-ship sorties each, (\d+) with under 4 sorties(?:; flights (\d+), refused for fuel (\d+))?")


def air(tags):
    """Aircraft fuel (AIRFUEL lines, SUPPLY-INVENTORY.md section 15): per tag, team and day window, mean over seeds:
    squadron fuel units taken at launch, returned, net, refilled from the pool (all per day), the stock left in four-ship sorties
    (per squadron, end of window), launches that found too little stock, and the pool fuel the net use would be if a squadron
    fuel unit were a pound (SUPPLY_PT_FUEL = 10000 lb a point, the sortie draw is lb / 100) against what the refineries make a day."""
    wins = [(24, 72), (72, 168), (168, 240)]
    print("%-10s %-5s %-8s %5s %9s %9s %9s %9s %8s %7s %9s %9s %7s %7s" % ("tag", "team", "hours", "seeds", "taken/d", "returned", "net/d", "refilled", "sorties", "short", "net@lb/d", "refin/d", "flts/d", "refused"))
    for tag in tags:
        for team, name in TEAMS:
            rows = {}
            runs = {}
            for s in range(1, 13):
                fr = load(tag, s)
                if not fr:
                    continue
                runs[s] = fr
                try:
                    for l in open(R % (tag, s, "log"), errors="replace"):
                        m = AF.match(l)
                        if m and int(m[2]) == team:
                            rows.setdefault(s, []).append([int(m[k]) if m[k] else 0 for k in range(1, 14)])
                except OSError:
                    pass
            for lo, hi in wins:
                acc = []
                for s, rs in rows.items():
                    sel = [r for r in rs if lo < r[0] <= hi]
                    if len(sel) < 2 or runs[s][-1]["t"] / 60 < hi - 6:
                        continue
                    days = len(sel) * 6 / 24.0
                    taken, ret, ref = (sum(r[k] for r in sel) / days for k in (2, 3, 4))
                    short = sum(r[5] for r in sel)
                    prod = (at(runs[s], hi)["sp"][team][9] - at(runs[s], lo)["sp"][team][9]) / ((hi - lo) / 24.0)
                    flights, refused = (sum(r[k] for r in sel) / days for k in (11, 12))
                    acc.append((taken, ret, taken - ret, ref, sel[-1][9], short, (taken - ret) / 100.0, prod, flights, refused))
                if acc:
                    m = [mean(a[k] for a in acc) for k in range(10)]
                    print("%-10s %-5s %3d-%-4d %5d %9.0f %9.0f %9.0f %9.0f %8.0f %7.0f %9.0f %9.0f %7.0f %7.0f" % (tag, name[:5], lo, hi, len(acc), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9]))


def main():
    if sys.argv[1:2] == ["--air"]:
        air(sys.argv[2:])
        return

    if sys.argv[1:2] == ["--fuel"]:
        fuel(sys.argv[2:])
        return

    if sys.argv[1:2] == ["--score"]:
        score(sys.argv[2] if len(sys.argv) > 2 else "t1")
        return

    if sys.argv[1:2] == ["--table"]:
        table(sys.argv[2:])
        return
    tags = sys.argv[1:] or ["mod-s0"]
    for tag in tags:
        runs = [fr for fr in (load(tag, s) for s in range(1, 13)) if fr]
        if not runs:
            print("\n== %s: no runs" % tag)
            continue
        print("\n===== %s: %d seeds" % (tag, len(runs)))
        for team, name in TEAMS:
            print("  -- %s, mean of seeds at hour:" % name, "  ".join("%6d" % h for h in HRS))

            def row(label, f):
                print("     %-34s" % label, "  ".join("%6.0f" % mean(f(at(fr, h), fr, h) for fr in runs) for h in HRS))

            def day(i):   # per-day change of a cumulative counter in the 24 h to h
                return lambda f, fr, h: f["sp"][team][i] - at(fr, max(h - 24, 1))["sp"][team][i]

            row("battalion supply %", lambda f, fr, h: f["sp"][team][30])
            row("  front line (within 30 km) %", lambda f, fr, h: f["sp"][team][32])
            row("  battalions under 25 %", lambda f, fr, h: f["sp"][team][33])
            row("supply ratio x1000 (500 = cap)", lambda f, fr, h: f["sp"][team][5])
            row("fuel ratio x1000", lambda f, fr, h: f["sp"][team][6])
            row("supply pool", lambda f, fr, h: f["sp"][team][0])
            row("fuel pool", lambda f, fr, h: f["sp"][team][1])
            row("replacement pool", lambda f, fr, h: f["sp"][team][2])
            row("production capacity /day (rate)", lambda f, fr, h: f["sp"][team][21])
            row("supply produced, last 24 h", day(8))
            row("supply sent to units, last 24 h", day(11))
            row("fuel produced, last 24 h", day(9))
            row("fuel sent to units, last 24 h", day(13))
            row("repl produced, last 24 h", day(10))
            row("repl to ground, last 24 h", day(18))
            row("ground vehicles", lambda f, fr, h: f["s"][team][0])
            row("objectives held", lambda f, fr, h: f["s"][team][4])
        wins = [events(tag, s).get(17) for s in range(1, 13)]
        wins = [w for w in wins if w is not None]
        print("  Blue win event (17) fired in %d of %d runs; mean hour %.0f" % (len(wins), len(runs), mean(wins)))


main()
