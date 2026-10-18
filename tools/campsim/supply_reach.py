"""supply_reach.py <run> [<run> ...]: battalion supply against distance to the nearest friendly supply source (depot, army base, rail terminal, port),
for Blue (teams 1-3) and the North (6) / China (5), every 24 h of a campsim timeline. Answers: does supply depend on where a unit is?

Objective ownership is the meta line's start owner plus each frame's "o" rows ([id, team, status] changes)."""
import json
import math
import sys

SOURCES = {3, 10, 19, 23}   # TYPE_ARMYBASE, TYPE_DEPOT, TYPE_PORT, TYPE_RAIL_TERMINAL
BINS = (0, 25, 50, 100, 150, 250, 9999)
SIDES = {"Blue": (1, 2, 3), "North": (6,), "China": (5,)}


def side_of(team):
    for k, v in SIDES.items():
        if team in v:
            return k


def main():
    for run in sys.argv[1:]:
        lines = open(run if run.endswith(".jsonl") else run + ".jsonl").readlines()
        meta = json.loads(lines[0])
        objs = {o[0]: o for o in meta["objs"]}
        owner = {o[0]: o[4] for o in meta["objs"]}
        src = [o for o in meta["objs"] if o[3] in SOURCES]
        print("%s: %d supply sources (%s)" % (run, len(src), ", ".join("%d type %d" % (sum(1 for o in src if o[3] == t), t) for t in sorted(SOURCES))))
        print("  hour side   n  | mean supply by km to nearest own source: " + " ".join("%d-%d" % (BINS[i], BINS[i + 1]) for i in range(len(BINS) - 1)) +
              " | share of bns >100 km | supply <50: n")
        nexth = 0
        for l in lines[1:]:
            if not l.startswith('{"t"'):
                continue
            f = json.loads(l)
            for row in f.get("o") or []:
                owner[row[0]] = row[1]
            h = f["t"] / 60
            if h < nexth:
                continue
            nexth += 24
            for sd, teams in SIDES.items():
                mine = [(objs[i][1], objs[i][2]) for i in owner if owner[i] in teams and objs[i][3] in SOURCES]
                bns = [u for u in f["u"] if u[2] in teams and u[1] == 0]
                if not bns or not mine:
                    continue
                cells = [[] for _ in range(len(BINS) - 1)]
                far = 0
                for u in bns:
                    d = min(math.hypot(u[3] - x, u[4] - y) for x, y in mine)
                    far += d > 100
                    for i in range(len(BINS) - 1):
                        if BINS[i] <= d < BINS[i + 1]:
                            cells[i].append(u[11])
                print("  %4d %-5s %4d | %s | %3.0f%% | %d" % (h, sd, len(bns), " ".join(("%5.0f(%d)" % (sum(c) / len(c), len(c))) if c else "    -   " for c in cells),
                                                       100.0 * far / len(bns), sum(1 for u in bns if u[11] < 50)))


if __name__ == "__main__":
    main()
