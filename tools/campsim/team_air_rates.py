"""Aircraft fuel use of every team that owns squadrons, from the AIRFUEL lines (campsim exe13 and later).

    python team_air_rates.py TAG

Per team and day window, mean over the seeds: squadrons and aircraft, flights per day, units drawn at launch per day and per aircraft per day,
net units used per day (drawn minus returned) and what the team's refineries make (pool points per day, sp row 9) to set against it.
A pool point is 100 units in the capped-stock mode, so the net use in points is the net units / 100.
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AF = re.compile(r"AIRFUEL h=(\d+) team (\d+): taken (\d+) returned (\d+) refilled (\d+), (\d+) launches with too little stock .*?; "
                r"(\d+) squadrons, (\d+) aircraft, mean stock (\d+) = (\d+) four-ship sorties each, (\d+) with under 4 sorties; flights (\d+)")
WINS = [(0, 24), (24, 72), (72, 168), (168, 240)]


def mean(v):
    v = list(v)
    return sum(v) / len(v) if v else 0.0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return

    tag = sys.argv[1]
    rows = {}
    prod = {}

    for f in sorted(glob.glob(os.path.join(HERE, "runs", "save0-%s-s*.log" % tag))):
        if f.endswith("ffdebug.log"):
            continue

        seed = f.rsplit("-s", 1)[1].split(".")[0]

        for line in open(f, errors="ignore"):
            m = AF.match(line)
            if m:
                rows.setdefault((int(m[2]), seed), []).append([int(m[k]) for k in (1, 3, 4, 8, 7, 12)])

        try:
            lines = open(f[:-3] + "jsonl")
            json.loads(next(lines))
            fr = [o for o in map(json.loads, lines) if "t" in o]

            for team in {t for t, _ in rows}:
                prod[(team, seed)] = fr
        except OSError:
            pass

    print("%-4s %-9s %5s %5s %5s %8s %9s %9s %9s %8s %9s" % ("team", "days", "seeds", "sqdn", "ac", "flts/d", "drawn/d", "per ac/d", "net/d", "net pts", "refin/d"))

    for team in sorted({t for t, _ in rows}):
        for lo, hi in WINS:
            acc = []

            for (t, seed), rs in rows.items():
                if t != team:
                    continue

                sel = [r for r in rs if lo < r[0] <= hi]

                if len(sel) < 3:
                    continue

                days = len(sel) * 6 / 24.0
                taken = sum(r[1] for r in sel) / days
                ret = sum(r[2] for r in sel) / days
                aircraft = mean(r[3] for r in sel)
                sqdn = mean(r[4] for r in sel)
                flights = sum(r[5] for r in sel) / days
                fr = prod.get((team, seed))
                make = 0.0

                if fr:
                    a = min(fr, key=lambda x: abs(x["t"] / 60 - lo))
                    b = min(fr, key=lambda x: abs(x["t"] / 60 - hi))
                    make = (b["sp"][team][9] - a["sp"][team][9]) / ((hi - lo) / 24.0)

                acc.append((sqdn, aircraft, flights, taken, taken / aircraft if aircraft else 0, taken - ret, (taken - ret) / 100.0, make))

            if acc:
                m = [mean(a[k] for a in acc) for k in range(8)]
                print("%-4d %3d-%-5d %5d %5.0f %5.0f %8.0f %9.0f %9.0f %9.0f %8.0f %9.0f" % (team, lo, hi, len(acc), *m))


main()
