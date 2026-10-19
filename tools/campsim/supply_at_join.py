"""supply_at_join.py ARM_PREFIX [...]: the North's supply when China joins the war, per run.

For each runs/chdd-sx-<arm>-s<seed>.jsonl: the hour China (team 5) enters (results.jsonl china_in_h),
and the DPRK (team 6) supply chain frame nearest that hour -- battalions' mean supply, the mean of those
within 30 km of the front, how many are under 25% -- plus the same at hour 0, and the outcome.

    python supply_at_join.py PC_          # every round-29 arm
"""
import glob
import json
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "supply", "results.jsonl")


def frames(path):
    out = []
    with open(path) as f:
        f.readline()
        for line in f:
            try:
                fr = json.loads(line)
            except ValueError:
                continue
            if "sp" in fr:
                out.append(fr)
    return out


def north(fr):
    v = fr["sp"][6]
    return v[29], v[30], v[31], v[32], v[33]   # battalions, mean supply, at the front, their mean, under 25%


def main():
    prefixes = sys.argv[1:] or ["PC_"]
    res = {}
    for line in open(RES):
        r = json.loads(line)
        res[(r["arm"], r["seed"])] = r
    rows = defaultdict(list)
    for path in sorted(glob.glob(os.path.join(HERE, "runs", "chdd-sx-*-s*.jsonl"))):
        m = re.match(r"chdd-sx-(.+)-s(\d+)\.jsonl$", os.path.basename(path))
        if not m or not any(m.group(1).startswith(p) for p in prefixes):
            continue
        arm, seed = m.group(1), int(m.group(2))
        r = res.get((arm, seed))
        if not r:
            continue                       # still running
        fs = frames(path)
        if not fs:
            continue
        join = r.get("china_in_h")
        hours = [f["t"] / 60.0 for f in fs]  # t is campaign minutes
        at = fs[min(range(len(fs)), key=lambda i: abs(hours[i] - join))] if join is not None else None
        rows[arm].append((seed, r, north(fs[0]), north(at) if at else None, join))

    print("%-10s %4s | %-9s %5s | North at h0: bns sup front frSup <25 | at China's entry: bns sup front frSup <25"
          % ("arm", "seed", "end", "share"))
    for arm in sorted(rows):
        for seed, r, n0, nj, join in sorted(rows[arm]):
            end = "%s@%d" % (r.get("endgame"), r.get("end_h") or 0)
            j = "h%-4s" % (join if join is not None else "-")
            print("%-10s %4d | %-9s %5.1f | %3d %3d %3d %3d %3d | %s %3d %3d %3d %3d %3d" % (
                (arm, seed, end, r["share_end"]) + n0 + ((j,) + nj if nj else (j, 0, 0, 0, 0, 0))))
        js = [x[3] for x in rows[arm] if x[3]]
        if js:
            print("%-10s mean | wins %d/%d, share %.1f | at China's entry: mean supply %.0f, front %.0f, under 25%% %.1f" % (
                arm, sum(1 for x in rows[arm] if x[1].get("blue_win", x[1].get("endgame") == 17)), len(rows[arm]),
                sum(x[1]["share_end"] for x in rows[arm]) / len(rows[arm]),
                sum(j[1] for j in js) / len(js), sum(j[3] for j in js) / len(js), sum(j[4] for j in js) / len(js)))
        print()


if __name__ == "__main__":
    main()
