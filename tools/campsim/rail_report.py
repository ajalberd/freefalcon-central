"""Compare rail variants across seeds (rail-tracks branch).

    python rail_report.py escal-norail escal-pyongbu escal-alllines

Each argument is a run-name prefix in runs/ (<prefix>-s<seed>.jsonl/.log/.ffdebug.log). Prints, per
variant, medians over seeds: when the war ended (event 17) and when China / Russia joined, DPRK
ground losses and the share to air, DPRK supply (trigger statistic) at h24/h48, and from the
engine's rail log: trains spawned and destroyed per side, railhead arrivals, supply and fuel
handed over, bridges reported down and stranded runs.
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


def one(path):
    log = open(path + ".log", errors="replace").read()
    ev = {int(a): int(b) for a, b in re.findall(r"EVENT (\d+) FIRED at min (\d+)", log)}
    frames = [json.loads(l) for l in open(path + ".jsonl") if l.startswith('{"t"')]
    if not frames:
        return None
    last = frames[-1]

    def at(h):
        return min(frames, key=lambda f: abs(f["t"] - h * 60))

    lo = last.get("lo")  # lo[team] = [gnd<-air, gnd<-art, gnd<-gnd, gnd<-naval, air<-air, air<-gnd]
    dprk_gnd = sum(lo[DPRK][:4]) if lo else None
    r = {
        "win": ev.get(17), "china": ev.get(11), "russia": ev.get(12),
        "dprk_gnd_lost": dprk_gnd,
        "air_share": 100.0 * lo[DPRK][0] / dprk_gnd if lo and dprk_gnd else None,
        "blue_gnd_lost": sum(sum(lo[t][:4]) for t in BLUE) if lo else None,
        "sup24": at(24)["st"][DPRK][0], "sup48": at(48)["st"][DPRK][0],
        "days": last["t"] / 1440.0,
    }
    dbg = path + ".ffdebug.log"
    rail = open(dbg, errors="replace").read() if os.path.exists(dbg) else ""
    spawned = re.findall(r"spawned train \d+ for team (\d+)", rail)
    r["spawn_red"] = sum(1 for t in spawned if int(t) in (4, 5, 6))
    r["spawn_blue"] = len(spawned) - r["spawn_red"]
    r["destroyed"] = len(re.findall(r"-- train destroyed", rail))
    arr = re.findall(r"reached the railhead .*?: (\d+) supply, (\d+) fuel to (\d+) unit", rail)
    r["arrivals"] = len(arr)
    r["supply"] = sum(int(a[0]) for a in arr)
    r["fuel"] = sum(int(a[1]) for a in arr)
    r["empty_arrivals"] = sum(1 for a in arr if a[2] == "0")
    r["bridges_down"] = len(set(re.findall(r"bridge at km [\d.]+ \((.*?), objective \d+.*?is DOWN", rail)))
    r["stranded"] = len(re.findall(r"STRANDED", rail))
    so = re.findall(r"troops -- so far (\d+) trips weighed: (\d+) by rail.*?(\d+) trains all in use\); "
                    r"(\d+) arrived, (\d+) stopped short, (\d+) got off", rail)
    last = so[-1] if so else ("0",) * 6
    r["t_rode"], r["t_full"], r["t_arrived"], r["t_stopped"], r["t_contact"] = (
        int(last[1]), int(last[2]), int(last[3]), int(last[4]), int(last[5]))
    # China: battalions of team 5 in the first frame -- net km moved, and km to the nearest
    # Blue-held objective at the end (how close to the front it got)
    first = frames[0]
    china = {u[0]: (u[3], u[4]) for u in first["u"] if u[1] == 0 and u[2] == 5}
    end = {u[0]: u for u in last_frame_units(frames) if u[1] == 0}
    moved = [((end[i][3] - x) ** 2 + (end[i][4] - y) ** 2) ** 0.5 for i, (x, y) in china.items() if i in end]
    r["china_moved"] = statistics.median(moved) if moved else None
    r["china_alive"] = 100.0 * sum(end[i][5] for i in china if i in end) / max(1, sum(
        u[5] for u in first["u"] if u[0] in china))
    return r


def last_frame_units(frames):
    return frames[-1]["u"]


def med(vals, fmt="%.0f"):
    v = [x for x in vals if x is not None]
    if not v:
        return "-"
    s = fmt % statistics.median(v)
    if len(v) < len(vals):
        s += " (%d/%d)" % (len(v), len(vals))
    return s


def hours(vals):
    v = [x for x in vals if x is not None]
    if not v:
        return "never"
    s = "h%.0f (h%.0f-%.0f)" % (statistics.median(v) / 60, min(v) / 60, max(v) / 60)
    if len(v) < len(vals):
        s += " %d/%d" % (len(v), len(vals))
    return s


def main():
    rows = []
    for prefix in sys.argv[1:]:
        runs = [one(p[:-6]) for p in sorted(glob.glob(os.path.join(RUNS, prefix + "-s*.jsonl")))]
        runs = [r for r in runs if r]
        if not runs:
            print("%s: no runs" % prefix)
            continue
        col = lambda k: [r[k] for r in runs]
        rows.append((prefix, len(runs), [
            ("war ends", hours(col("win"))),
            ("China joins", hours(col("china"))),
            ("Russia joins", hours(col("russia"))),
            ("DPRK ground lost", med(col("dprk_gnd_lost"))),
            ("  of it to air %", med(col("air_share"))),
            ("Blue ground lost", med(col("blue_gnd_lost"))),
            ("DPRK supply% h24", med(col("sup24"))),
            ("DPRK supply% h48", med(col("sup48"))),
            ("trains spawned Red/Blue", "%s / %s" % (med(col("spawn_red")), med(col("spawn_blue")))),
            ("trains destroyed", med(col("destroyed"))),
            ("railhead arrivals", med(col("arrivals"))),
            ("  with nobody to supply", med(col("empty_arrivals"))),
            ("supply / fuel handed over", "%s / %s" % (med(col("supply")), med(col("fuel")))),
            ("rail bridges reported down", med(col("bridges_down"))),
            ("stranded runs", med(col("stranded"))),
            ("troop rides", med(col("t_rode"))),
            ("  arrived / stopped short / ground contact", "%s / %s / %s" % (
                med(col("t_arrived")), med(col("t_stopped")), med(col("t_contact")))),
            ("  refused: all trains in use", med(col("t_full"))),
            ("China km moved (median bn)", med(col("china_moved"))),
            ("China vehicles left %", med(col("china_alive"))),
        ]))
    if not rows:
        return
    w = max(len(k) for k, _ in rows[0][2])
    print(("%-*s" % (w, "")) + "".join("  %-22s" % ("%s (%d)" % (p, n)) for p, n, _ in rows))
    for i, (k, _) in enumerate(rows[0][2]):
        print(("%-*s" % (w, k)) + "".join("  %-22s" % r[2][i][1] for r in rows))


if __name__ == "__main__":
    main()
