"""Summarise Iron Fortress arms for the balance work.

    python iron_arms.py [tag ...]        (default: every save1* arm found in runs/)

One line per seed and a median row per arm: outcome, days, Blue/DPRK objectives at the end, Blue and
DPRK ground vehicles against their start, ownership changes. Reads the timelines directly, so it needs
no saved summaries. Mirrors run_batch.summarise but keeps what the balance question needs.
"""
import glob
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.path.join(HERE, "runs")


def one(path):
    frames, fin, meta = [], None, None
    for ln in open(path, errors="replace"):
        try:
            o = json.loads(ln)
        except Exception:
            break
        if o.get("final"):
            fin = o
        elif o.get("meta"):
            meta = o
        else:
            frames.append(o)
    if not frames or not meta:
        return None
    a, b = frames[0], frames[-1]
    teams = meta.get("teams", [])
    blue = next((i for i, n in enumerate(teams) if n in ("ROK", "Israel")), 2)
    red = next((i for i, n in enumerate(teams) if n in ("DPRK", "Egypt")), 6)
    flips = sum(len(f.get("o", [])) for f in frames[1:])
    g0, g1 = a["s"][blue][0], b["s"][blue][0]
    r0, r1 = a["s"][red][0], b["s"][red][0]
    return dict(days=b["t"] / 1440.0, endgame=(fin or {}).get("endgame"),
                bobj0=a["s"][blue][4], bobj=b["s"][blue][4], robj=b["s"][red][4],
                bg=(100.0 * g1 / max(1, g0)), rg=(100.0 * r1 / max(1, r0)),
                flips=flips)


def arm(tag):
    if ":" in tag:                             # "save1b:bground" -> save1b, bground
        save, tag = tag.split(":", 1)
    else:
        save = "save1"
    paths = sorted(glob.glob(os.path.join(RUNS, "%s%s-s[0-9]*.jsonl"
                                          % (save, ("-" + tag) if tag else ""))))
    rows = []
    for p in paths:
        r = one(p)
        if r:
            rows.append((os.path.basename(p), r))
    return rows


def med(rows, key):
    return statistics.median([r[key] for _, r in rows]) if rows else float("nan")


def main():
    tags = sys.argv[1:]
    if not tags:
        tags = sorted({os.path.basename(p)[6:-len("-s1.jsonl")]
                       for p in glob.glob(os.path.join(RUNS, "save1*-s1.jsonl"))})
        tags = [t if t else "baseline" for t in tags]

    print("%-14s %-5s %-6s %-9s %-13s %-11s %-8s" %
          ("arm", "blu", "dprk", "days", "Blue objs", "DPRK objs", "Blue gnd"))
    for tag in tags:
        rows = arm("" if tag == "baseline" else tag)
        if not rows:
            print("%-14s (no runs)" % tag)
            continue
        bw = sum(1 for _, r in rows if r["endgame"] == 17)
        rw = sum(1 for _, r in rows if r["endgame"] == 14)
        print("%-14s %-5s %-6s %-9s %-13s %-11s %-8s" % (
            tag, "%d/%d" % (bw, len(rows)), "%d/%d" % (rw, len(rows)),
            "%.1f-%.1f" % (min(r["days"] for _, r in rows), max(r["days"] for _, r in rows)),
            "%d-%d (%d)" % (min(r["bobj"] for _, r in rows), max(r["bobj"] for _, r in rows), med(rows, "bobj")),
            "%d-%d" % (min(r["robj"] for _, r in rows), max(r["robj"] for _, r in rows)),
            "%.0f%%" % med(rows, "bg")))
    print()
    print("%-14s %-9s %-10s %-9s" % ("arm", "DPRK gnd", "flips", "seeds"))
    for tag in tags:
        rows = arm("" if tag == "baseline" else tag)
        if not rows:
            continue
        print("%-14s %-9s %-10s %-9d" % (
            tag, "%.0f%%" % med(rows, "rg"),
            "%d-%d" % (min(r["flips"] for _, r in rows), max(r["flips"] for _, r in rows)), len(rows)))


if __name__ == "__main__":
    main()
