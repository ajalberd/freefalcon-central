"""What China's (team 5) starting battalions do in a run: python china_report.py <run name> [<run name> ...]

Follows the PRC battalions present in the first frame by unit id (when China joins they are handed to DPRK, team 6).  Per time: how many are
alive and how many vehicles (of full strength), how far each is from the nearest Blue battalion (km, 1 grid = 1 km) as quartiles, how many are within
30 / 60 / 120 km of one, moving, their orders (RESERVE / CAPTURE / DEFEND / other), mean supply and morale, and how many have a destination
south of where they stand.  Unit record: [id, kind, team, x, y, veh, full, moving, morale, orders, tactic, supply, objId, objX, objY, destX, destY].
"""
import json, math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ORD = {0: "RES", 1: "CAP", 2: "SEC", 6: "DEF", 7: "SUP"}


def q(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * (len(v) - 1) + 0.5))] if v else 0


def report(name):
    frames = [json.loads(l) for l in open(os.path.join(HERE, "runs", name + ".jsonl")) if l.startswith('{"t"')]
    ids = {u[0] for u in frames[0]["u"] if u[2] == 5 and u[1] <= 1}
    print("\n%s: %d PRC battalions at the start" % (name, len(ids)))
    print("  hour  alive  veh/full   dist to nearest Blue bn: q25 med q75 | <30  <60  <120 km | moving  orders (RES CAP DEF oth)  supply morale  dest south")
    for h in (0, 6, 12, 24, 36, 48, 72, 96, 120):
        f = min(frames, key=lambda f: abs(f["t"] - h * 60))
        blue = [(u[3], u[4]) for u in f["u"] if u[2] == 2 and u[1] <= 1]
        b = [u for u in f["u"] if u[0] in ids]
        if not b or not blue:
            continue
        d = [min(math.hypot(u[3] - x, u[4] - y) for x, y in blue) for u in b]
        o = {"RES": 0, "CAP": 0, "DEF": 0, "oth": 0}
        for u in b:
            o[ORD.get(u[9], "oth") if ORD.get(u[9]) in o else "oth"] += 1
        south = sum(1 for u in b if u[16] and u[16] < u[4] - 5)
        print("  h%-4d %4d  %5d/%-5d            %4.0f %4.0f %4.0f | %3d %4d %4d | %4d      %3d %3d %3d %3d       %3.0f   %3.0f   %3d" % (
            h, len(b), sum(u[5] for u in b), sum(u[6] for u in b), q(d, .25), q(d, .5), q(d, .75),
            sum(1 for x in d if x < 30), sum(1 for x in d if x < 60), sum(1 for x in d if x < 120),
            sum(1 for u in b if u[7]), o["RES"], o["CAP"], o["DEF"], o["oth"],
            sum(u[11] for u in b) / len(b), sum(u[8] for u in b) / len(b), south))


if __name__ == "__main__":
    for n in sys.argv[1:]:
        report(n)
