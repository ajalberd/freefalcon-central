"""supply_arc.py [arm ...]: the shape of each war -- does Blue advance, lose ground, and come back? From the daily Blue objective counts in
supply/results.jsonl: the biggest loss from a running peak (setback), the recovery after that trough, wins, share, China's join hour and
Blue's share of time on the offensive after it. An "arc" seed has a setback of 40+ objectives followed by a recovery of 40+."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def arc(daily):
    best_drop, at = 0, None
    peak = daily[0]
    for i, v in enumerate(daily):
        peak = max(peak, v)
        if peak - v > best_drop:
            best_drop, at = peak - v, i
    rec = (max(daily[at:]) - daily[at]) if at is not None else 0
    return best_drop, at, rec


def main():
    rows = {}
    for l in open(os.path.join(HERE, "supply", "results.jsonl")):
        r = json.loads(l)
        if "error" not in r and r.get("blue_objs_daily"):
            rows.setdefault(r["arm"], []).append(r)
    arms = sys.argv[1:] or sorted(rows)
    print("%-12s wins | share | arc seeds (setback>=40 then recovery>=40) | per seed: end, setback/day, recovery | China in, Blue off%% after" % "arm")
    for a in arms:
        v = sorted(rows.get(a, []), key=lambda r: r["seed"])
        if not v:
            continue
        cells, arcs = [], 0
        for r in v:
            d, at, rec = arc(r["blue_objs_daily"])
            arcs += d >= 40 and rec >= 40
            cells.append("%s%s -%d/d%s +%d" % ("W" if r.get("blue_win", r.get("endgame") == 17) else "-", int(r["end_h"]), d, at if at is not None else "-", rec))
        print("%-12s %d/%d | %4.1f | %d | %s | %s" % (a, sum(1 for r in v if r.get("blue_win", r.get("endgame") == 17)), len(v), sum(r["share_end"] for r in v) / len(v), arcs,
                                              "  ".join(cells), " ".join("%s/%s" % (r.get("china_in_h"), r.get("blue_off_after_join")) for r in v)))


if __name__ == "__main__":
    main()
