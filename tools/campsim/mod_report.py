"""Compare campaign variants (e.g. the DPRK Air Boost mod) across seeds.

    python mod_report.py a_none-mod a_half-mod a_air-mod

Each argument is a run-name prefix in runs/ (<prefix>-s<seed>.jsonl/.log). Prints, per variant,
the mean over seeds of: when China/Russia/"We win" fired, DPRK flights airborne per day, what DPRK
air destroyed (Blue ground vehicles, Blue aircraft), DPRK aircraft lost, and DPRK ground left.
"""
import glob
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.path.join(HERE, "runs")
BLUE, DPRK = (1, 2, 3), 6


def one(prefix_path):
    log = open(prefix_path + ".log", errors="replace").read()
    ev = {int(a): int(b) for a, b in re.findall(r"EVENT (\d+) FIRED at min (\d+)", log)}
    frames = [json.loads(l) for l in open(prefix_path + ".jsonl") if l.startswith('{"t"')]
    if not frames:
        return None
    last = frames[-1]
    # flights airborne: kind 4 units; per day = mean count over that day's hourly frames
    per_day = {}
    for f in frames:
        d = f["t"] // 1440
        per_day.setdefault(d, []).append(sum(1 for u in f["u"] if u[1] == 4 and u[2] == DPRK))

    def at(h):
        return min(frames, key=lambda f: abs(f["t"] - h * 60))

    lo = last.get("lo")
    s0, s24 = frames[0]["s"], at(24)["s"]
    return {
        "china": ev.get(11), "russia": ev.get(12), "win": ev.get(17),
        "flights": [statistics.mean(per_day[d]) for d in sorted(per_day)],
        # lo[team] = [gnd<-air, gnd<-art, gnd<-gnd, gnd<-naval, air<-air, air<-gnd]
        "blue_gnd_by_air": sum(lo[t][0] for t in BLUE) if lo else None,
        "blue_air_by_air": sum(lo[t][4] for t in BLUE) if lo else None,
        "dprk_air_lost": (lo[DPRK][4] + lo[DPRK][5]) if lo else None,
        "dprk_gnd_left_h24": 100.0 * s24[DPRK][0] / max(1, s0[DPRK][0]),
        "days": last["t"] / 1440.0,
        "crash": "CRASH at" in log,
    }


def fmt_h(vals):
    v = [x for x in vals if x is not None]
    if not v:
        return "never"
    s = "h%.0f" % (statistics.mean(v) / 60)
    if len(v) < len(vals):
        s += " (%d/%d)" % (len(v), len(vals))
    return s


def mean(vals, f="%.0f"):
    v = [x for x in vals if x is not None]
    return f % statistics.mean(v) if v else "-"


def main(prefixes):
    print("| | " + " | ".join(prefixes) + " |")
    print("|---|" + "---|" * len(prefixes))
    res = {}
    for p in prefixes:
        rs = [one(os.path.splitext(x)[0]) for x in sorted(glob.glob(os.path.join(RUNS, p + "-s*.jsonl")))]
        res[p] = [r for r in rs if r]
    rows = [
        ("runs (crashed)", lambda rs: "%d (%d)" % (len(rs), sum(r["crash"] for r in rs))),
        ("China joins", lambda rs: fmt_h([r["china"] for r in rs])),
        ("Russia joins", lambda rs: fmt_h([r["russia"] for r in rs])),
        ("Blue wins (17)", lambda rs: fmt_h([r["win"] for r in rs])),
        ("DPRK flights airborne, day 1", lambda rs: mean([r["flights"][0] for r in rs], "%.1f")),
        ("DPRK flights airborne, day 3", lambda rs: mean([r["flights"][2] if len(r["flights"]) > 2 else None for r in rs], "%.1f")),
        ("DPRK flights airborne, day 5", lambda rs: mean([r["flights"][4] if len(r["flights"]) > 4 else None for r in rs], "%.1f")),
        ("Blue ground vehicles killed by air", lambda rs: mean([r["blue_gnd_by_air"] for r in rs])),
        ("Blue aircraft killed air-to-air", lambda rs: mean([r["blue_air_by_air"] for r in rs])),
        ("DPRK aircraft lost", lambda rs: mean([r["dprk_air_lost"] for r in rs])),
        ("DPRK ground left at h24", lambda rs: mean([r["dprk_gnd_left_h24"] for r in rs], "%.0f%%")),
    ]
    for name, fn in rows:
        print("| %s | %s |" % (name, " | ".join(fn(res[p]) if res[p] else "-" for p in prefixes)))


if __name__ == "__main__":
    main(sys.argv[1:] or ["a_none-mod", "a_half-mod", "a_air-mod"])
