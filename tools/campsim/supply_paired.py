"""supply_paired.py [base_arm] [arm ...]: exact same-seed comparisons (det=1 runs replay identically) of each arm against the base arm.

Per seed: the hour the two wars first differ (a strike should show its hour), each side's end (win hour, or 'none' at 240 h), Blue's share at
the end, and the differences. A war not won counts as 240 h for the difference."""
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "supply", "results.jsonl")


def sigs(arm, seed):
    p = os.path.join(HERE, "runs", "chdd-sx-%s-s%d.jsonl" % (arm, seed))
    return [hashlib.md5(json.dumps(json.loads(l)["u"]).encode()).hexdigest() for l in open(p) if l.startswith('{"t"')]


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "P_S"
    rows = {}
    for l in open(RES):
        r = json.loads(l)
        if "error" not in r:
            rows[(r["arm"], r["seed"])] = r
    arms = sys.argv[2:] or sorted({a for a, _ in rows if a.startswith("P_") and a != base})
    seeds = sorted(s for a, s in rows if a == base)
    for arm in arms:
        print("%s vs %s" % (arm, base))
        dw, ds, wins = [], [], 0
        for s in seeds:
            x, b = rows.get((arm, s)), rows.get((base, s))
            if not x or not b:
                continue
            ax, bx = sigs(arm, s), sigs(base, s)
            part = next((i for i, (p, q) in enumerate(zip(ax, bx)) if p != q), None)
            wx = x["end_h"] if x["endgame"] == 17 else 240.0
            wb = b["end_h"] if b["endgame"] == 17 else 240.0
            wins += x["endgame"] == 17
            dw.append(wx - wb)
            ds.append(x["share_end"] - b["share_end"])
            print("  seed %d  wars part at h%-4s | %s %-6s vs %-6s (%+5.0f h) | Blue share %.1f vs %.1f (%+.1f)" % (
                s, part if part is not None else "-", arm, ("h%.0f" % wx) if x["endgame"] == 17 else "none", ("h%.0f" % wb) if b["endgame"] == 17 else "none",
                wx - wb, x["share_end"], b["share_end"], ds[-1]))
        if dw:
            better = sum(1 for d in dw if d < 0)
            worse = sum(1 for d in dw if d > 0)
            base_wins = sum(1 for s in seeds if rows.get((base, s)) and rows[(base, s)]["endgame"] == 17 and (arm, s) in rows)
            print("  => Blue wins %d/%d (base %d); win hour %+.0f h on average (earlier in %d seeds, later in %d); share %+.1f points on average, "
                  "lower in %d of %d seeds" % (wins, len(dw), base_wins, sum(dw) / len(dw), better, worse, sum(ds) / len(ds),
                                                sum(1 for d in ds if d < -0.5), len(ds)))


if __name__ == "__main__":
    main()
