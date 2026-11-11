"""Regression gate for Taiwan-theatre work.

Per standing instruction: every change made to help the Taiwan campaign gets a three-seed
check that Korea "Rolling Fire" still plays properly -- a majority of seeds must be a Blue win
by day 10.
Rolling Fire is the stock Korea save0; Blue is ROK + U.S. (teams 1 and 2).

    python regress_taiwan.py            # seeds 1, 2 and 3, ten days
    python regress_taiwan.py --seeds 1 2 3

Exit code 0 when a MAJORITY of seeds is a Blue win by day 10, 1 otherwise.
"""
import argparse
import os
import re
import sys

import run_batch

BLUE_WIN = 17          # the Korea save0 endgame code for a Blue victory
DAY10_H = 240.0


def final_of(name):
    log = os.path.join("runs", name + ".log")
    with open(log, "r", encoding="latin-1", errors="replace") as fh:
        tail = fh.read()[-200000:]
    m = re.findall(r"^RESULT seed=\d+ endgame=(\d+) engine=\d+ campaign_h=([\d.]+)", tail, re.M)
    return (int(m[-1][0]), float(m[-1][1])) if m else (None, None)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", nargs="+", default=["1", "2", "3"])
    ap.add_argument("--days", type=int, default=10)
    ap.add_argument("--jobs", type=int, default=3)
    a = ap.parse_args(argv)
    tag = "korea-gate"

    cmd = [sys.executable, "-u", "run_batch.py", "--save", "save0", "--days", str(a.days),
           "--seeds", *a.seeds, "--jobs", str(len(a.seeds)), "--tag", tag,
           "--set", "usecfg=1"]
    print("running:", " ".join(cmd))
    import subprocess
    rc = subprocess.call(cmd)

    wins = 0
    rows = []
    for seed in a.seeds:
        end_, hours = final_of("save0-%s-s%s" % (tag, seed))
        ok = end_ == BLUE_WIN and hours is not None and hours <= DAY10_H
        wins += 1 if ok else 0
        rows.append((seed, end_, hours, ok))
        print("  seed %s: endgame=%s at %.1f h  %s"
              % (seed, end_, hours if hours is not None else -1, "OK" if ok else "NOT BY DAY 10"))

    need = len(a.seeds) // 2 + 1          # a majority, not every seed
    if wins >= need:
        print("PASS: Blue won %d of %d seeds by day 10 (majority)" % (wins, len(a.seeds)))
        return 0
    print("FAIL: Blue won only %d of %d seeds by day 10 (need %d)"
          % (wins, len(a.seeds), need))
    return 1


if __name__ == "__main__":
    sys.exit(main())
