"""supply_levers.py [base_arm] [arm ...]: what each supply lever does to units' supply, and where, against the same seeds of the base
(det=1 pairs). Per arm, averaged over seeds: Blue spearhead supply (battalions within 40 km of Blue's northern edge) on days 3 / 5 / 7, the
North's front supply the same way, Blue battalions under 50% on day 5, Blue's share at the end; each as arm - base."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KEYS = [("blue_spear_sup_d3", "Blue spear d3"), ("blue_spear_sup_d5", "d5"), ("blue_spear_sup_d7", "d7"),
        ("north_front_sup_d3", "North front d3"), ("north_front_sup_d5", "d5"), ("north_front_sup_d7", "d7"),
        ("blue_low_sup_d5", "Blue bns <50 d5"), ("share_end", "Blue share")]


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "P_h"
    rows = {}
    for l in open(os.path.join(HERE, "supply", "results.jsonl")):
        r = json.loads(l)
        if "error" not in r:
            rows[(r["arm"], r["seed"])] = r
    arms = sys.argv[2:] or sorted({a for a, _ in rows if a.startswith("L_")})
    print("%-16s %s | Blue wins (base)" % ("arm vs " + base, " | ".join("%s" % n for _, n in KEYS)))
    for arm in arms:
        diffs = {k: [] for k, _ in KEYS}
        wins = bw = n = 0
        for (a, s), x in rows.items():
            b = rows.get((base, s))
            if a != arm or not b:
                continue
            n += 1
            wins += x.get("blue_win", x["endgame"] == 17)
            bw += b.get("blue_win", b["endgame"] == 17)
            for k, _ in KEYS:
                if x.get(k) is not None and b.get(k) is not None:
                    diffs[k].append(x[k] - b[k])
        if not n:
            continue
        print("%-16s %s | %d/%d (%d)" % (arm, " | ".join(("%+6.1f" % (sum(v) / len(v))) if v else "     -" for v in diffs.values()), wins, n, bw))


if __name__ == "__main__":
    main()
